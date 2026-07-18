#include "../hotfix/bridge/av-governor.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using mcb::AVGovernor;
using mcb::AVGovernorDecision;
using mcb::AVGovernorPhase;

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
} // namespace

int main()
{
	AVGovernor governor;
	governor.configure(true, 120, 120, 100, true, 40, 10000, 3,
		250, 10000, 2000, 8);

	// A source with incompatible settings is deliberately bypassed.
	auto decision = governor.process_audio(900 * ms, 900 * ms);
	assert(decision.accept && decision.output_timestamp_ns == 900 * ms);
	decision = governor.process_video(900 * ms, 900 * ms);
	assert(decision.accept && decision.output_timestamp_ns == 900 * ms);

	governor.set_source_configured(true);
	assert(governor.snapshot().phase == AVGovernorPhase::WarmingUp);

	// Startup holds both paths while it learns a robust baseline over time.
	auto startup = feed_pairs(governor, 1000 * ms, 1000 * ms, 24, 20 * static_cast<int64_t>(ms));
	auto state = governor.snapshot();
	assert(state.locked);
	assert(state.baseline_valid);
	assert(state.baseline_samples >= 3);
	assert(state.lock_acquisitions == 1);
	assert(state.raw_av_skew_ns != 0);

	// Both streams are mapped to the same future OBS playout timeline.
	decision = governor.process_video(startup.next_video_source, startup.next_arrival, 50001, 60001);
	assert(decision.accept);
	state = governor.snapshot();
	assert(state.video_playout_depth_ns > 80 * static_cast<int64_t>(ms));
	assert(state.video_playout_depth_ns < 130 * static_cast<int64_t>(ms));
	decision = governor.process_audio(startup.next_video_source + 20 * ms,
		startup.next_arrival + ms, 50002, 60002);
	assert(decision.accept);
	state = governor.snapshot();
	assert(state.audio_playout_depth_ns > 80 * static_cast<int64_t>(ms));
	assert(state.audio_playout_depth_ns < 130 * static_cast<int64_t>(ms));

	// Persistent rate drift is distinguished from jitter before video-only correction begins.
	const uint64_t drift_video = startup.next_video_source + frame_ns;
	const uint64_t drift_arrival = startup.next_arrival + frame_ns;
	auto drift = feed_pairs(governor, drift_video, drift_arrival, 560,
		20 * static_cast<int64_t>(ms), 600, 70000);
	state = governor.snapshot();
	assert(state.locked);
	assert(state.drift_samples >= 24);
	assert(state.drift_confidence >= 70);
	assert(state.drift_ppm > 100);
	assert(state.correction_updates > 0);
	assert(state.video_correction_ns != 0);

	// A video stall allows one controlled fade-out packet, then gates further audio.
	const uint64_t stalled_audio_source = drift.next_video_source + 20 * ms;
	const uint64_t stalled_arrival = drift.next_arrival + 200 * ms;
	decision = governor.process_audio(stalled_audio_source, stalled_arrival, 88001, 98001);
	assert(decision.accept);
	assert(decision.audio_gain_start == 1.0f && decision.audio_gain_end == 0.0f);
	state = governor.snapshot();
	assert(state.video_stalled);
	assert(state.phase == AVGovernorPhase::Holding);
	assert(state.fade_out_packets >= 1);
	assert(!governor.process_audio(stalled_audio_source + frame_ns,
		stalled_arrival + frame_ns, 88002, 98002).accept);

	// Recovery learns a fresh baseline and resumes both paths atomically with one fade-in.
	const uint64_t recovery_video_source = drift.next_video_source + 2 * frame_ns;
	const uint64_t recovery_arrival = stalled_arrival + frame_ns;
	auto recovery = feed_pairs(governor, recovery_video_source, recovery_arrival, 24,
		20 * static_cast<int64_t>(ms), 0, 100000);
	state = governor.snapshot();
	assert(state.locked);
	assert(state.atomic_recoveries >= 1);
	assert(recovery.saw_audio_fade_in);
	assert(state.fade_in_packets >= 1);

	// A backward source timestamp starts a new epoch instead of entering OBS.
	const uint64_t previous_video_output = state.last_output_video_ns;
	assert(!governor.process_video(1000 * ms, recovery.next_arrival + ms, 120001, 130001).accept);
	state = governor.snapshot();
	assert(state.phase == AVGovernorPhase::Holding);
	assert(state.discontinuities >= 2);

	// The fresh epoch is rebased onto a monotonic OBS timeline.
	auto rebased = feed_pairs(governor, 1000 * ms, recovery.next_arrival + 2 * ms, 24,
		20 * static_cast<int64_t>(ms), 0, 140000);
	state = governor.snapshot();
	assert(state.locked);
	decision = governor.process_video(rebased.next_video_source, rebased.next_arrival, 150001, 160001);
	assert(decision.accept);
	assert(decision.output_timestamp_ns > previous_video_output);
	assert(governor.snapshot().epoch_rebase_ns != 0);

	const std::string csv = governor.flight_recorder_csv();
	assert(csv.find("arrival_ns,source_ns") == 0);
	assert(csv.find("ndi_timestamp_100ns") != std::string::npos);
	assert(csv.find("HOLD") != std::string::npos);
	assert(csv.find("LOCK") != std::string::npos);
	assert(csv.find("FADE") != std::string::npos);
	assert(csv.find("150001") != std::string::npos);

	governor.set_source_configured(false);
	assert(governor.process_audio(1, 1).accept);
	governor.configure(false, 120, 120, 100, true, 40, 1000, 3);
	assert(governor.process_video(1, 1).accept);

	std::cout << "A/V Governor 1.2 tests passed\n";
	return 0;
}
