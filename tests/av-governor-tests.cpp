#include "../hotfix/bridge/av-governor.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using mcb::AVGovernor;
using mcb::AVGovernorDecision;
using mcb::AVGovernorPhase;
using mcb::AVGovernorReason;

namespace {
constexpr uint64_t ms = 1000000ULL;
constexpr uint64_t frame_ns = 16000000ULL;

struct FeedResult {
	uint64_t next_video_source = 0;
	uint64_t next_arrival = 0;
	AVGovernorDecision last_audio{};
	AVGovernorDecision last_video{};
	bool saw_audio_fade_in = false;
};

FeedResult feed_pairs(AVGovernor &governor, uint64_t video_source, uint64_t arrival,
	uint32_t pairs, int64_t base_audio_offset_ns, int64_t drift_ppm = 0,
	int64_t raw_seed = 1000)
{
	FeedResult result;
	for (uint32_t i = 0; i < pairs; ++i) {
		const uint64_t elapsed = static_cast<uint64_t>(i) * frame_ns;
		const int64_t drift_ns = static_cast<int64_t>(
			(static_cast<long double>(elapsed) * static_cast<long double>(drift_ppm)) / 1000000.0L);
		const uint64_t video_ts = video_source + elapsed;
		const uint64_t audio_ts = static_cast<uint64_t>(static_cast<int64_t>(video_ts) + base_audio_offset_ns + drift_ns);
		const uint64_t video_arrival = arrival + elapsed;
		const uint64_t audio_arrival = video_arrival + ms;
		result.last_video = governor.process_video(video_ts, video_arrival,
			raw_seed + static_cast<int64_t>(i) * 2, raw_seed + 10000 + static_cast<int64_t>(i) * 2);
		result.last_audio = governor.process_audio(audio_ts, audio_arrival,
			raw_seed + static_cast<int64_t>(i) * 2 + 1, raw_seed + 10001 + static_cast<int64_t>(i) * 2);
		if (result.last_audio.accept && result.last_audio.audio_gain_start == 0.0f &&
			result.last_audio.audio_gain_end == 1.0f)
			result.saw_audio_fade_in = true;
	}
	result.next_video_source = video_source + static_cast<uint64_t>(pairs) * frame_ns;
	result.next_arrival = arrival + static_cast<uint64_t>(pairs) * frame_ns;
	return result;
}

void configure(AVGovernor &governor)
{
	governor.configure(true, 120, 120, 100, true, 40, 1000, 12,
		5000, 60000, 30000, 8);
	governor.set_source_configured(true);
}

FeedResult acquire_initial_lock(AVGovernor &governor)
{
	configure(governor);
	auto startup = feed_pairs(governor, 1000 * ms, 1000 * ms, 360,
		20 * static_cast<int64_t>(ms));
	const auto state = governor.snapshot();
	assert(state.locked);
	assert(state.baseline_valid);
	assert(state.baseline_skew_ns > 15 * static_cast<int64_t>(ms));
	assert(state.baseline_skew_ns < 25 * static_cast<int64_t>(ms));
	return startup;
}
} // namespace

int main()
{
	// A source with incompatible settings is deliberately bypassed.
	AVGovernor bypassed;
	auto decision = bypassed.process_audio(900 * ms, 900 * ms);
	assert(decision.accept && decision.output_timestamp_ns == 900 * ms);

	// Slow persistent drift matures over at least 30 seconds before correction begins.
	AVGovernor drift_governor;
	auto startup = acquire_initial_lock(drift_governor);
	const int64_t trusted_baseline = drift_governor.snapshot().baseline_skew_ns;
	auto drift = feed_pairs(drift_governor, startup.next_video_source, startup.next_arrival,
		2400, 20 * static_cast<int64_t>(ms), 600, 70000);
	auto state = drift_governor.snapshot();
	assert(state.locked);
	assert(state.baseline_skew_ns == trusted_baseline);
	assert(state.drift_samples >= 100);
	assert(state.drift_confidence >= 75);
	assert(state.drift_ppm > 100);
	assert(state.correction_updates > 0);
	assert(state.video_correction_ns != 0);

	// A short video stall preserves the trusted baseline, quarantines recovery
	// samples, and verifies the recovered candidate before locking again.
	const uint64_t stalled_audio_source = state.last_audio_source_ns + frame_ns;
	const uint64_t stalled_arrival = drift.next_arrival + 200 * ms;
	decision = drift_governor.process_audio(stalled_audio_source, stalled_arrival, 88001, 98001);
	assert(decision.accept);
	state = drift_governor.snapshot();
	assert(state.phase == AVGovernorPhase::Holding);
	assert(state.baseline_valid);
	assert(state.baseline_skew_ns == trusted_baseline);
	auto recovered = feed_pairs(drift_governor, drift.next_video_source + 2 * frame_ns,
		stalled_arrival + frame_ns, 480, 20 * static_cast<int64_t>(ms), 0, 100000);
	state = drift_governor.snapshot();
	assert(state.locked);
	assert(state.baseline_skew_ns == trusted_baseline);
	assert(state.atomic_recoveries >= 1);
	assert(state.quarantined_samples > 0);
	assert(recovered.saw_audio_fade_in);

	// A post-jump timing candidate roughly 100 ms away is never learned as the
	// new normal. Protection fails open and retains the known-good reference.
	AVGovernor mismatch_governor;
	auto clean = acquire_initial_lock(mismatch_governor);
	const int64_t known_good = mismatch_governor.snapshot().baseline_skew_ns;
	assert(!mismatch_governor.process_video(1000 * ms, clean.next_arrival + ms, 120001, 130001).accept);
	auto mismatch = feed_pairs(mismatch_governor, 1000 * ms, clean.next_arrival + 2 * ms,
		480, -80 * static_cast<int64_t>(ms), 0, 140000);
	(void)mismatch;
	state = mismatch_governor.snapshot();
	assert(state.phase == AVGovernorPhase::Failed);
	assert(state.reason == AVGovernorReason::BaselineMismatch);
	assert(state.fail_safe_bypassed);
	assert(state.baseline_valid);
	assert(state.baseline_skew_ns == known_good);
	assert(state.candidate_baseline_skew_ns < -70 * static_cast<int64_t>(ms));

	// Routine telemetry cannot evict the critical discontinuity and failure.
	feed_pairs(mismatch_governor, 6000 * ms, clean.next_arrival + 6000 * ms,
		5000, 20 * static_cast<int64_t>(ms), 0, 200000);
	const std::string csv = mismatch_governor.flight_recorder_csv();
	assert(csv.find("trusted_baseline_ms") != std::string::npos);
	assert(csv.find("clock_domain_offset_ms") != std::string::npos);
	assert(csv.find("HOLD") != std::string::npos);
	assert(csv.find("BASELINE_MISMATCH") != std::string::npos);
	assert(csv.find("FAILED") != std::string::npos);
	assert(mismatch_governor.snapshot().recorder_events <= 1024);

	drift_governor.set_source_configured(false);
	assert(drift_governor.process_audio(1, 1).accept);

	std::cout << "A/V Governor 1.3 trusted-baseline, drift, recovery, and recorder tests passed\n";
	return 0;
}
