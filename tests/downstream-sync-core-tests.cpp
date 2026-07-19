#include "downstream-sync-core.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(1);
	}
}

uint64_t drifting_audio_timestamp(uint64_t start, uint64_t elapsed, double error_ppm)
{
	const long double scale = 1.0L - static_cast<long double>(error_ppm) * 1.0e-6L;
	return start + static_cast<uint64_t>(std::llround(static_cast<long double>(elapsed) * scale));
}

} // namespace

int main()
{
	constexpr uint64_t step_ns = 250000000ULL;
	constexpr uint64_t start_ns = 1000000000ULL;
	constexpr uint32_t sample_rate = 48000;
	constexpr double native_error_ppm = 22.222222; // 200 ms late after 2.5 hours

	// The alpha 5 actuator owns one fractional clock for the complete
	// four-channel packet. Validate its long-run frame total and hard reset.
	mcb::LinkedAudioPacketClock packet_clock;
	constexpr uint32_t packet_frames = 1024;
	constexpr size_t packet_count = 3000;
	uint64_t packet_timestamp = start_ns;
	uint64_t timestamp_remainder = 0;
	uint64_t previous_output_timestamp = 0;
	mcb::LinkedAudioPacketPlan packet_plan{};
	for (size_t i = 0; i < packet_count; ++i) {
		packet_plan = packet_clock.plan(packet_frames, packet_timestamp,
			sample_rate, -30.0, true);
		require(packet_plan.output_frames == packet_frames ||
			packet_plan.output_frames + 1 == packet_frames,
			"negative PPM produced an invalid packet-size change");
		require(!previous_output_timestamp ||
			packet_plan.output_timestamp_ns > previous_output_timestamp,
			"corrected source packet timestamps did not remain monotonic");
		previous_output_timestamp = packet_plan.output_timestamp_ns;
		const uint64_t numerator = static_cast<uint64_t>(packet_frames) *
			mcb::DownstreamSyncCore::kNsPerSecond + timestamp_remainder;
		packet_timestamp += numerator / sample_rate;
		timestamp_remainder = numerator % sample_rate;
	}
	const long double expected_adjustment = -30.0e-6L *
		static_cast<long double>(packet_frames) * static_cast<long double>(packet_count);
	require(std::fabs(static_cast<long double>(packet_plan.net_frame_adjustment) -
		expected_adjustment) <= 1.0L,
		"shared packet clock did not accumulate the requested PPM rate");
	packet_clock.reset(9000000000ULL, sample_rate);
	packet_plan = packet_clock.plan(packet_frames, 9000000000ULL,
		sample_rate, 0.0, true);
	require(packet_plan.output_frames == packet_frames &&
		packet_plan.output_timestamp_ns == 9000000000ULL &&
		packet_plan.net_frame_adjustment == 0,
		"hard NDI epoch reset retained packet-clock correction state");

	mcb::DownstreamSyncCore core;
	core.configure(true, 1000, 100, 4, 5000, 120000, 30000);

	long double cumulative_frame_adjustment = 0.0L;
	double last_correction_ppm = 0.0;
	int64_t net_frames = 0;
	uint64_t prior_output_ts = start_ns;
	const uint64_t duration_ns = 9000ULL * mcb::DownstreamSyncCore::kNsPerSecond;
	for (uint64_t elapsed = step_ns; elapsed <= duration_ns; elapsed += step_ns) {
		const uint64_t wall = start_ns + elapsed;
		const uint64_t video_ts = start_ns + elapsed;
		const uint64_t raw_audio_ts = drifting_audio_timestamp(start_ns, elapsed, native_error_ppm);
		cumulative_frame_adjustment +=
			static_cast<long double>(sample_rate) * static_cast<long double>(step_ns) /
			static_cast<long double>(mcb::DownstreamSyncCore::kNsPerSecond) *
			static_cast<long double>(last_correction_ppm) * 1.0e-6L;
		net_frames = static_cast<int64_t>(std::llround(cumulative_frame_adjustment));
		const uint64_t output_ts = raw_audio_ts;
		core.observe_video(video_ts, wall);
		core.observe_audio_input(raw_audio_ts, wall);
		core.report_audio_output(output_ts, wall, net_frames, sample_rate);
		core.tick(wall);
		last_correction_ppm = core.correction_ppm();
		prior_output_ts = output_ts;
	}

	auto snapshot = core.snapshot();
	require(snapshot.phase == mcb::DownstreamSyncPhase::Locked, "controller did not lock");
	require(snapshot.baseline_valid, "trusted baseline was not established");
	require(snapshot.confidence >= 75, "long drift did not become verified");
	require(snapshot.native_audio_error_ppm >= 18 && snapshot.native_audio_error_ppm <= 27,
		"native 22 ppm audio error was not measured downstream");
	require(snapshot.raw_deviation_ns >= 180000000 && snapshot.raw_deviation_ns <= 220000000,
		"raw relation did not retain drift from the first trusted baseline");
	require(std::fabs(static_cast<double>(snapshot.corrected_deviation_ns) / 1e6) < 12.0,
		"linked correction did not keep actual output near the trusted sync");
	require(snapshot.correction_ppm < -10.0 && snapshot.correction_ppm > -100.0,
		"late audio did not receive a bounded speed-up command");

	// A raw input re-anchor must preserve the already-corrected output timeline.
	const uint64_t incident_wall = start_ns + duration_ns + step_ns;
	core.observe_video(incident_wall, incident_wall);
	core.observe_audio_input(incident_wall + 500000000ULL, incident_wall);
	core.report_audio_output(prior_output_ts + step_ns, incident_wall, net_frames, sample_rate);
	core.tick(incident_wall);
	snapshot = core.snapshot();
	require(snapshot.phase == mcb::DownstreamSyncPhase::Verifying,
		"timestamp incident did not enter trusted-reference verification");
	require(snapshot.baseline_valid, "timestamp incident discarded the first trusted baseline");

	// Automatic recovery may retain the trusted reference, but no pre-restart
	// observation may be considered fresh in the new receiver epoch.
	core.reset(false, true);
	snapshot = core.snapshot();
	require(snapshot.baseline_valid && snapshot.phase == mcb::DownstreamSyncPhase::Verifying,
		"automatic recovery did not retain the trusted reference for verification");
	core.tick(incident_wall + step_ns);
	snapshot = core.snapshot();
	require(!snapshot.measurement_fresh,
		"pre-restart clock observations leaked into the new verification epoch");

	// A manual/configuration restart intentionally trusts the newly rebuilt NDI
	// receiver instead of comparing it with an obsolete timing epoch.
	core.reset(false, false);
	snapshot = core.snapshot();
	require(!snapshot.baseline_valid && snapshot.phase == mcb::DownstreamSyncPhase::Learning,
		"manual receiver restart did not discard the old trusted reference");
	require(snapshot.drift_samples == 0 && snapshot.raw_deviation_ns == 0 &&
		snapshot.corrected_deviation_ns == 0,
		"manual receiver restart retained learned drift data");
	require(snapshot.correction_ppm == 0.0 && snapshot.target_ppm == 0.0 &&
		snapshot.net_frame_adjustment == 0,
		"manual receiver restart retained an old PPM command or frame adjustment");

	std::cout << "Downstream Sync Core tests passed\n";
	return 0;
}
