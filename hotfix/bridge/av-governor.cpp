#include "av-governor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace mcb {

uint64_t AVGovernor::ns_from_ms(int value)
{
	return static_cast<uint64_t>(std::max(value, 0)) * 1000000ULL;
}

uint64_t AVGovernor::magnitude(int64_t value)
{
	if (value >= 0)
		return static_cast<uint64_t>(value);
	return static_cast<uint64_t>(-(value + 1)) + 1ULL;
}

bool AVGovernor::source_discontinuity(uint64_t previous, uint64_t current)
{
	if (!previous || !current)
		return false;
	if (current < previous)
		return previous - current > 5000000ULL;
	return current - previous > 1500000000ULL;
}

int64_t AVGovernor::median(std::array<int64_t, kBaselineCapacity> values, size_t count)
{
	if (!count)
		return 0;
	count = std::min(count, values.size());
	std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
	const size_t middle = count / 2;
	if ((count & 1U) != 0U)
		return values[middle];
	return values[middle - 1] / 2 + values[middle] / 2;
}

int64_t AVGovernor::median_small(std::array<int64_t, kSkewFilterCapacity> values, size_t count)
{
	if (!count)
		return 0;
	count = std::min(count, values.size());
	std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
	const size_t middle = count / 2;
	if ((count & 1U) != 0U)
		return values[middle];
	return values[middle - 1] / 2 + values[middle] / 2;
}

uint64_t AVGovernor::median_intervals(std::array<uint64_t, kVideoIntervalCapacity> values, size_t count)
{
	if (!count)
		return 0;
	count = std::min(count, values.size());
	std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
	return values[count / 2];
}

const char *AVGovernor::phase_name(AVGovernorPhase phase)
{
	switch (phase) {
	case AVGovernorPhase::Bypassed: return "BYPASSED";
	case AVGovernorPhase::WarmingUp: return "WARMING_UP";
	case AVGovernorPhase::Locked: return "LOCKED";
	case AVGovernorPhase::Holding: return "HOLDING";
	case AVGovernorPhase::Relocking: return "RELOCKING";
	}
	return "UNKNOWN";
}

const char *AVGovernor::reason_name(AVGovernorReason reason)
{
	switch (reason) {
	case AVGovernorReason::None: return "NONE";
	case AVGovernorReason::Startup: return "STARTUP";
	case AVGovernorReason::VideoStall: return "VIDEO_STALL";
	case AVGovernorReason::AudioDiscontinuity: return "AUDIO_DISCONTINUITY";
	case AVGovernorReason::VideoDiscontinuity: return "VIDEO_DISCONTINUITY";
	case AVGovernorReason::AudioNonMonotonic: return "AUDIO_NON_MONOTONIC";
	case AVGovernorReason::VideoNonMonotonic: return "VIDEO_NON_MONOTONIC";
	case AVGovernorReason::SkewExceeded: return "SKEW_EXCEEDED";
	case AVGovernorReason::PlayoutDepthExceeded: return "PLAYOUT_DEPTH_EXCEEDED";
	case AVGovernorReason::SourceReconfigured: return "SOURCE_RECONFIGURED";
	case AVGovernorReason::ManualReset: return "MANUAL_RESET";
	}
	return "UNKNOWN";
}

const char *AVGovernor::event_name(EventType type)
{
	switch (type) {
	case EventType::Sample: return "SAMPLE";
	case EventType::Hold: return "HOLD";
	case EventType::Lock: return "LOCK";
	case EventType::Correction: return "CORRECTION";
	case EventType::Drop: return "DROP";
	case EventType::Reset: return "RESET";
	case EventType::Fade: return "FADE";
	}
	return "UNKNOWN";
}

const char *AVGovernor::stream_name(Stream stream)
{
	return stream == Stream::Audio ? "AUDIO" : "VIDEO";
}

void AVGovernor::configure(bool enabled, int max_deviation_ms, int video_stall_ms,
	int playout_delay_ms, bool drift_correction, int max_video_correction_ms,
	int correction_slew_ppm, int relock_pairs, int baseline_window_ms,
	int drift_window_ms, int drift_minimum_ms, int drift_deadband_ppm)
{
	std::lock_guard<std::mutex> lock(mutex_);
	state_.enabled = enabled;
	state_.drift_correction_enabled = drift_correction;
	state_.playout_delay_ns = ns_from_ms(std::clamp(playout_delay_ms, 40, 500));
	max_deviation_ns_ = ns_from_ms(std::clamp(max_deviation_ms, 40, 500));
	video_stall_ns_ = ns_from_ms(std::clamp(video_stall_ms, 60, 1000));
	max_video_correction_ns_ = ns_from_ms(std::clamp(max_video_correction_ms, 0, 120));
	correction_slew_ppm_ = static_cast<uint32_t>(std::clamp(correction_slew_ppm, 50, 10000));
	relock_required_ = static_cast<uint32_t>(std::clamp(relock_pairs, 3, 60));
	baseline_window_ns_ = ns_from_ms(std::clamp(baseline_window_ms, 250, 5000));
	const int bounded_drift_window_ms = std::clamp(drift_window_ms, 5000, 120000);
	drift_window_ns_ = ns_from_ms(bounded_drift_window_ms);
	drift_minimum_ns_ = ns_from_ms(std::clamp(drift_minimum_ms, 2000, bounded_drift_window_ms));
	drift_deadband_ppm_ = static_cast<uint32_t>(std::clamp(drift_deadband_ppm, 1, 250));
	state_.relock_required = relock_required_;
	state_.baseline_window_ns = baseline_window_ns_;
	state_.drift_window_ns = drift_window_ns_;
	state_.drift_minimum_ns = drift_minimum_ns_;
	acquisition_limit_ns_ = std::clamp<uint64_t>(max_deviation_ns_ * 2ULL, 250000000ULL, 500000000ULL);
	reset_locked(false, AVGovernorReason::ManualReset);
}

void AVGovernor::set_source_configured(bool configured)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_.source_configured == configured)
		return;
	state_.source_configured = configured;
	reset_locked(false, configured ? AVGovernorReason::Startup : AVGovernorReason::SourceReconfigured);
}

void AVGovernor::reset(bool reset_counters)
{
	std::lock_guard<std::mutex> lock(mutex_);
	reset_locked(reset_counters, AVGovernorReason::ManualReset);
}

void AVGovernor::reset_locked(bool reset_counters, AVGovernorReason reason)
{
	const bool enabled = state_.enabled;
	const bool configured = state_.source_configured;
	const bool correction_enabled = state_.drift_correction_enabled;
	const uint64_t playout_delay = state_.playout_delay_ns;
	const uint64_t audio_packets = reset_counters ? 0 : state_.audio_packets;
	const uint64_t video_frames = reset_counters ? 0 : state_.video_frames;
	const uint64_t blocked_audio = reset_counters ? 0 : state_.blocked_audio;
	const uint64_t blocked_video = reset_counters ? 0 : state_.blocked_video;
	const uint64_t discontinuities = reset_counters ? 0 : state_.discontinuities;
	const uint64_t lock_acquisitions = reset_counters ? 0 : state_.lock_acquisitions;
	const uint64_t recoveries = reset_counters ? 0 : state_.atomic_recoveries;
	const uint64_t correction_updates = reset_counters ? 0 : state_.correction_updates;
	const uint64_t monotonic_clamps = reset_counters ? 0 : state_.monotonic_clamps;
	const uint64_t fade_out_packets = reset_counters ? 0 : state_.fade_out_packets;
	const uint64_t fade_in_packets = reset_counters ? 0 : state_.fade_in_packets;
	const uint64_t epoch = reset_counters ? 0 : state_.epoch + 1;

	state_ = {};
	state_.enabled = enabled;
	state_.source_configured = configured;
	state_.drift_correction_enabled = correction_enabled;
	state_.playout_delay_ns = playout_delay;
	state_.baseline_window_ns = baseline_window_ns_;
	state_.drift_window_ns = drift_window_ns_;
	state_.drift_minimum_ns = drift_minimum_ns_;
	state_.audio_packets = audio_packets;
	state_.video_frames = video_frames;
	state_.blocked_audio = blocked_audio;
	state_.blocked_video = blocked_video;
	state_.discontinuities = discontinuities;
	state_.lock_acquisitions = lock_acquisitions;
	state_.atomic_recoveries = recoveries;
	state_.correction_updates = correction_updates;
	state_.monotonic_clamps = monotonic_clamps;
	state_.fade_out_packets = fade_out_packets;
	state_.fade_in_packets = fade_in_packets;
	state_.epoch = epoch;
	state_.last_output_audio_ns = last_output_audio_ns_;
	state_.last_output_video_ns = last_output_video_ns_;
	state_.relock_required = relock_required_;
	state_.phase = enabled && configured ? AVGovernorPhase::WarmingUp : AVGovernorPhase::Bypassed;
	state_.reason = enabled && configured ? reason : AVGovernorReason::None;

	audio_sequence_ = 0;
	video_sequence_ = 0;
	paired_audio_sequence_ = 0;
	paired_video_sequence_ = 0;
	baseline_start_arrival_ns_ = 0;
	baseline_count_ = 0;
	skew_filter_write_ = 0;
	skew_filter_count_ = 0;
	trend_write_ = 0;
	trend_count_ = 0;
	last_trend_sample_ns_ = 0;
	video_interval_write_ = 0;
	video_interval_count_ = 0;
	previous_video_for_interval_ns_ = 0;
	fade_in_pending_ = false;
	last_record_sample_ns_ = 0;
	if (reset_counters) {
		event_write_ = 0;
		event_count_ = 0;
	}
	record_locked(EventType::Reset, Stream::Audio, false, 0, 0, 0, 0, 0, true);
}

void AVGovernor::enter_hold_locked(AVGovernorReason reason, uint64_t arrival_ns)
{
	++state_.discontinuities;
	++state_.epoch;
	state_.phase = AVGovernorPhase::Holding;
	state_.reason = reason;
	state_.locked = false;
	state_.video_stalled = reason == AVGovernorReason::VideoStall;
	state_.baseline_valid = false;
	state_.raw_av_skew_ns = 0;
	state_.av_skew_ns = 0;
	state_.baseline_skew_ns = 0;
	state_.baseline_deviation_ns = 0;
	state_.video_correction_ns = 0;
	state_.target_video_correction_ns = 0;
	state_.drift_ppm = 0;
	state_.drift_confidence = 0;
	state_.epoch_rebase_ns = 0;
	state_.relock_progress = 0;
	state_.baseline_samples = 0;
	state_.drift_samples = 0;
	state_.last_audio_source_ns = 0;
	state_.last_video_source_ns = 0;
	state_.last_audio_arrival_ns = 0;
	state_.last_video_arrival_ns = 0;
	audio_sequence_ = 0;
	video_sequence_ = 0;
	paired_audio_sequence_ = 0;
	paired_video_sequence_ = 0;
	baseline_start_arrival_ns_ = arrival_ns;
	baseline_count_ = 0;
	skew_filter_write_ = 0;
	skew_filter_count_ = 0;
	trend_write_ = 0;
	trend_count_ = 0;
	last_trend_sample_ns_ = 0;
	fade_in_pending_ = true;
}

void AVGovernor::update_skew_locked()
{
	if (!state_.last_audio_source_ns || !state_.last_video_source_ns ||
		!state_.last_audio_arrival_ns || !state_.last_video_arrival_ns)
		return;
	const uint64_t now = std::max(state_.last_audio_arrival_ns, state_.last_video_arrival_ns);
	const uint64_t audio_advance = now - state_.last_audio_arrival_ns;
	const uint64_t video_advance = now - state_.last_video_arrival_ns;
	const uint64_t audio_projected = state_.last_audio_source_ns + audio_advance;
	const uint64_t video_projected = state_.last_video_source_ns + video_advance;
	state_.raw_av_skew_ns = static_cast<int64_t>(audio_projected) - static_cast<int64_t>(video_projected);
}

void AVGovernor::push_skew_filter_locked(int64_t skew_ns)
{
	skew_filter_[skew_filter_write_] = skew_ns;
	skew_filter_write_ = (skew_filter_write_ + 1) % skew_filter_.size();
	skew_filter_count_ = std::min(skew_filter_count_ + 1, skew_filter_.size());
	std::array<int64_t, kSkewFilterCapacity> ordered{};
	for (size_t i = 0; i < skew_filter_count_; ++i)
		ordered[i] = skew_filter_[i];
	state_.av_skew_ns = median_small(ordered, skew_filter_count_);
	if (state_.baseline_valid)
		state_.baseline_deviation_ns = state_.av_skew_ns - state_.baseline_skew_ns;
}

bool AVGovernor::observe_pair_locked(uint64_t arrival_ns)
{
	if (!state_.last_audio_source_ns || !state_.last_video_source_ns)
		return false;
	if (audio_sequence_ == paired_audio_sequence_ || video_sequence_ == paired_video_sequence_)
		return false;

	paired_audio_sequence_ = audio_sequence_;
	paired_video_sequence_ = video_sequence_;
	update_skew_locked();
	if (magnitude(state_.raw_av_skew_ns) > acquisition_limit_ns_) {
		baseline_count_ = 0;
		baseline_start_arrival_ns_ = arrival_ns;
		state_.relock_progress = 0;
		state_.baseline_samples = 0;
		return false;
	}
	push_skew_filter_locked(state_.raw_av_skew_ns);

	if (state_.phase == AVGovernorPhase::Locked) {
		update_trend_locked(arrival_ns);
		return false;
	}

	state_.phase = AVGovernorPhase::Relocking;
	if (!baseline_start_arrival_ns_)
		baseline_start_arrival_ns_ = arrival_ns;
	if (baseline_count_ < baseline_samples_.size()) {
		baseline_samples_[baseline_count_] = state_.av_skew_ns;
		++baseline_count_;
	} else {
		std::move(baseline_samples_.begin() + 1, baseline_samples_.end(), baseline_samples_.begin());
		baseline_samples_.back() = state_.av_skew_ns;
	}
	state_.baseline_samples = static_cast<uint32_t>(baseline_count_);
	state_.relock_progress = static_cast<uint32_t>(std::min<size_t>(baseline_count_, relock_required_));
	const uint64_t elapsed = arrival_ns >= baseline_start_arrival_ns_
		? arrival_ns - baseline_start_arrival_ns_
		: 0;
	if (baseline_count_ < relock_required_ || elapsed < baseline_window_ns_)
		return false;

	state_.baseline_skew_ns = median(baseline_samples_, baseline_count_);
	state_.baseline_deviation_ns = state_.av_skew_ns - state_.baseline_skew_ns;
	state_.baseline_valid = true;
	state_.locked = true;
	state_.video_stalled = false;
	state_.phase = AVGovernorPhase::Locked;
	state_.reason = AVGovernorReason::None;
	state_.relock_progress = relock_required_;
	state_.video_correction_ns = 0;
	state_.target_video_correction_ns = 0;
	state_.drift_ppm = 0;
	state_.drift_confidence = 0;
	trend_write_ = 0;
	trend_count_ = 0;
	last_trend_sample_ns_ = 0;
	establish_epoch_mapping_locked();
	const bool recovering = state_.lock_acquisitions > 0;
	++state_.lock_acquisitions;
	if (recovering) {
		++state_.atomic_recoveries;
		fade_in_pending_ = true;
	}
	record_locked(EventType::Lock, Stream::Video, false, state_.last_video_source_ns,
		state_.last_video_arrival_ns, 0, state_.last_video_ndi_timestamp_100ns,
		state_.last_video_ndi_timecode_100ns, true);
	return true;
}

void AVGovernor::update_trend_locked(uint64_t arrival_ns)
{
	if (!state_.baseline_valid)
		return;
	if (last_trend_sample_ns_ && arrival_ns >= last_trend_sample_ns_ &&
		arrival_ns - last_trend_sample_ns_ < trend_sample_interval_ns_)
		return;
	last_trend_sample_ns_ = arrival_ns;
	trend_[trend_write_] = {arrival_ns, state_.baseline_deviation_ns};
	trend_write_ = (trend_write_ + 1) % trend_.size();
	trend_count_ = std::min(trend_count_ + 1, trend_.size());
	estimate_drift_locked();
}

void AVGovernor::estimate_drift_locked()
{
	if (trend_count_ < 3) {
		state_.drift_samples = static_cast<uint32_t>(trend_count_);
		state_.drift_ppm = 0;
		state_.drift_confidence = 0;
		return;
	}

	uint64_t newest = 0;
	for (size_t i = 0; i < trend_count_; ++i)
		newest = std::max(newest, trend_[i].arrival_ns);
	const uint64_t oldest_allowed = newest > drift_window_ns_ ? newest - drift_window_ns_ : 0;
	std::array<TrendSample, kTrendCapacity> samples{};
	size_t count = 0;
	for (size_t i = 0; i < trend_count_; ++i) {
		const TrendSample &sample = trend_[i];
		if (sample.arrival_ns >= oldest_allowed)
			samples[count++] = sample;
	}
	state_.drift_samples = static_cast<uint32_t>(count);
	if (count < 3)
		return;

	uint64_t first_arrival = samples[0].arrival_ns;
	uint64_t last_arrival = samples[0].arrival_ns;
	for (size_t i = 1; i < count; ++i) {
		first_arrival = std::min(first_arrival, samples[i].arrival_ns);
		last_arrival = std::max(last_arrival, samples[i].arrival_ns);
	}
	const uint64_t span = last_arrival - first_arrival;
	if (!span)
		return;

	long double sum_t = 0.0L;
	long double sum_y = 0.0L;
	long double sum_tt = 0.0L;
	long double sum_ty = 0.0L;
	for (size_t i = 0; i < count; ++i) {
		const long double t = static_cast<long double>(samples[i].arrival_ns - first_arrival);
		const long double y = static_cast<long double>(samples[i].deviation_ns);
		sum_t += t;
		sum_y += y;
		sum_tt += t * t;
		sum_ty += t * y;
	}
	const long double n = static_cast<long double>(count);
	const long double denominator = n * sum_tt - sum_t * sum_t;
	if (std::fabs(denominator) < 1.0L)
		return;
	const long double slope = (n * sum_ty - sum_t * sum_y) / denominator;
	const long double intercept = (sum_y - slope * sum_t) / n;
	long double residual_sum = 0.0L;
	for (size_t i = 0; i < count; ++i) {
		const long double t = static_cast<long double>(samples[i].arrival_ns - first_arrival);
		const long double predicted = intercept + slope * t;
		residual_sum += std::fabs(static_cast<long double>(samples[i].deviation_ns) - predicted);
	}
	const long double mean_residual_ns = residual_sum / n;
	long double ppm = slope * 1000000.0L;
	ppm = std::clamp(ppm, -100000.0L, 100000.0L);
	state_.drift_ppm = static_cast<int64_t>(std::llround(ppm));

	const long double span_score = std::min<long double>(1.0L,
		static_cast<long double>(span) / static_cast<long double>(drift_minimum_ns_));
	const long double sample_score = std::min<long double>(1.0L, static_cast<long double>(count) / 24.0L);
	const long double residual_score = std::clamp(1.0L - mean_residual_ns / 5000000.0L, 0.0L, 1.0L);
	const long double confidence = 100.0L * span_score * sample_score * residual_score;
	state_.drift_confidence = static_cast<uint32_t>(std::clamp<long double>(confidence, 0.0L, 100.0L));
}

void AVGovernor::update_video_interval_locked(uint64_t source_ns)
{
	if (previous_video_for_interval_ns_ && source_ns > previous_video_for_interval_ns_) {
		const uint64_t delta = source_ns - previous_video_for_interval_ns_;
		if (delta >= 5000000ULL && delta <= 100000000ULL) {
			video_intervals_[video_interval_write_] = delta;
			video_interval_write_ = (video_interval_write_ + 1) % video_intervals_.size();
			video_interval_count_ = std::min(video_interval_count_ + 1, video_intervals_.size());
			state_.estimated_video_interval_ns = median_intervals(video_intervals_, video_interval_count_);
		}
	}
	previous_video_for_interval_ns_ = source_ns;
}

void AVGovernor::update_video_correction_locked()
{
	if (!state_.drift_correction_enabled || !state_.baseline_valid) {
		state_.target_video_correction_ns = 0;
		state_.video_correction_ns = 0;
		return;
	}

	const bool confirmed = state_.drift_confidence >= 70 &&
		magnitude(state_.drift_ppm) >= static_cast<uint64_t>(drift_deadband_ppm_);
	constexpr int64_t offset_deadband_ns = 2000000LL;
	int64_t target = confirmed && magnitude(state_.baseline_deviation_ns) > static_cast<uint64_t>(offset_deadband_ns)
		? state_.baseline_deviation_ns
		: 0;
	const int64_t limit = static_cast<int64_t>(max_video_correction_ns_);
	target = std::clamp(target, -limit, limit);
	state_.target_video_correction_ns = target;

	const uint64_t frame_interval = state_.estimated_video_interval_ns
		? state_.estimated_video_interval_ns
		: 16666667ULL;
	const uint64_t max_step_u = std::max<uint64_t>(1, (frame_interval * correction_slew_ppm_) / 1000000ULL);
	const int64_t max_step = max_step_u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
		? std::numeric_limits<int64_t>::max()
		: static_cast<int64_t>(max_step_u);
	const int64_t delta = target - state_.video_correction_ns;
	const int64_t step = std::clamp(delta, -max_step, max_step);
	if (step != 0) {
		state_.video_correction_ns += step;
		++state_.correction_updates;
		record_locked(EventType::Correction, Stream::Video, true, state_.last_video_source_ns,
			state_.last_video_arrival_ns, state_.last_output_video_ns,
			state_.last_video_ndi_timestamp_100ns, state_.last_video_ndi_timecode_100ns,
			magnitude(step) >= 1000000ULL);
	}
}

void AVGovernor::establish_epoch_mapping_locked()
{
	const uint64_t source_anchor = state_.last_video_source_ns ? state_.last_video_source_ns
		: state_.last_audio_source_ns;
	const uint64_t arrival_anchor = state_.last_video_arrival_ns ? state_.last_video_arrival_ns
		: state_.last_audio_arrival_ns;
	const uint64_t max_i64 = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	if (!source_anchor || !arrival_anchor || source_anchor > max_i64 ||
		state_.playout_delay_ns > max_i64 - source_anchor) {
		state_.epoch_rebase_ns = 0;
		return;
	}

	const uint64_t previous_output = std::max(last_output_audio_ns_, last_output_video_ns_);
	uint64_t desired = arrival_anchor;
	if (desired <= std::numeric_limits<uint64_t>::max() - state_.playout_delay_ns)
		desired += state_.playout_delay_ns;
	if (previous_output && desired <= previous_output)
		desired = previous_output == std::numeric_limits<uint64_t>::max() ? previous_output : previous_output + 1;
	if (desired > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		state_.epoch_rebase_ns = 0;
		return;
	}
	const int64_t raw = static_cast<int64_t>(source_anchor) + static_cast<int64_t>(state_.playout_delay_ns);
	state_.epoch_rebase_ns = static_cast<int64_t>(desired) - raw;
}

uint64_t AVGovernor::adjusted_timestamp_locked(Stream stream, uint64_t source_ns, uint64_t arrival_ns)
{
	if (source_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		return source_ns;
	int64_t timestamp = static_cast<int64_t>(source_ns);
	const int64_t delay = static_cast<int64_t>(state_.playout_delay_ns);
	if (delay > 0 && timestamp <= std::numeric_limits<int64_t>::max() - delay)
		timestamp += delay;
	if ((state_.epoch_rebase_ns > 0 && timestamp <= std::numeric_limits<int64_t>::max() - state_.epoch_rebase_ns) ||
		(state_.epoch_rebase_ns < 0 && timestamp >= std::numeric_limits<int64_t>::min() - state_.epoch_rebase_ns))
		timestamp += state_.epoch_rebase_ns;
	if (stream == Stream::Video &&
		((state_.video_correction_ns > 0 && timestamp <= std::numeric_limits<int64_t>::max() - state_.video_correction_ns) ||
		 (state_.video_correction_ns < 0 && timestamp >= std::numeric_limits<int64_t>::min() - state_.video_correction_ns)))
		timestamp += state_.video_correction_ns;

	uint64_t output = timestamp > 0 ? static_cast<uint64_t>(timestamp) : 1ULL;
	uint64_t &last = stream == Stream::Audio ? last_output_audio_ns_ : last_output_video_ns_;
	if (last && output <= last) {
		output = last == std::numeric_limits<uint64_t>::max() ? last : last + 1;
		++state_.monotonic_clamps;
	}
	last = output;
	state_.last_output_audio_ns = last_output_audio_ns_;
	state_.last_output_video_ns = last_output_video_ns_;
	const int64_t depth = output >= arrival_ns
		? static_cast<int64_t>(output - arrival_ns)
		: -static_cast<int64_t>(arrival_ns - output);
	if (stream == Stream::Audio)
		state_.audio_playout_depth_ns = depth;
	else
		state_.video_playout_depth_ns = depth;
	return output;
}

bool AVGovernor::playout_depth_sane_locked(Stream stream, uint64_t output_ns, uint64_t arrival_ns)
{
	const int64_t depth = output_ns >= arrival_ns
		? static_cast<int64_t>(output_ns - arrival_ns)
		: -static_cast<int64_t>(arrival_ns - output_ns);
	const int64_t target = static_cast<int64_t>(state_.playout_delay_ns);
	const uint64_t tolerance = std::max<uint64_t>(250000000ULL, max_deviation_ns_);
	const bool sane = magnitude(depth - target) <= tolerance;
	if (!sane)
		state_.reason = AVGovernorReason::PlayoutDepthExceeded;
	if (stream == Stream::Audio)
		state_.audio_playout_depth_ns = depth;
	else
		state_.video_playout_depth_ns = depth;
	return sane;
}

AVGovernorDecision AVGovernor::process_locked(Stream stream, uint64_t source_ns, uint64_t arrival_ns,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
{
	if (!state_.enabled || !state_.source_configured) {
		state_.phase = AVGovernorPhase::Bypassed;
		return {true, source_ns, 1.0f, 1.0f};
	}

	if (stream == Stream::Audio) {
		++state_.audio_packets;
		state_.last_audio_ndi_timestamp_100ns = ndi_timestamp_100ns;
		state_.last_audio_ndi_timecode_100ns = ndi_timecode_100ns;
	} else {
		++state_.video_frames;
		state_.last_video_ndi_timestamp_100ns = ndi_timestamp_100ns;
		state_.last_video_ndi_timecode_100ns = ndi_timecode_100ns;
	}

	if (!source_ns) {
		if (stream == Stream::Audio)
			++state_.blocked_audio;
		else
			++state_.blocked_video;
		record_locked(EventType::Drop, stream, false, source_ns, arrival_ns, 0,
			ndi_timestamp_100ns, ndi_timecode_100ns, true);
		return {false, 0, 1.0f, 1.0f};
	}

	const uint64_t previous_source = stream == Stream::Audio
		? state_.last_audio_source_ns
		: state_.last_video_source_ns;
	if (source_discontinuity(previous_source, source_ns)) {
		enter_hold_locked(stream == Stream::Audio ? AVGovernorReason::AudioDiscontinuity
			: AVGovernorReason::VideoDiscontinuity, arrival_ns);
		if (stream == Stream::Audio)
			++state_.blocked_audio;
		else
			++state_.blocked_video;
		record_locked(EventType::Hold, stream, false, source_ns, arrival_ns, 0,
			ndi_timestamp_100ns, ndi_timecode_100ns, true);
		return {false, 0, 1.0f, 1.0f};
	}
	if (previous_source && source_ns <= previous_source) {
		state_.reason = stream == Stream::Audio ? AVGovernorReason::AudioNonMonotonic
			: AVGovernorReason::VideoNonMonotonic;
		if (stream == Stream::Audio)
			++state_.blocked_audio;
		else
			++state_.blocked_video;
		record_locked(EventType::Drop, stream, false, source_ns, arrival_ns, 0,
			ndi_timestamp_100ns, ndi_timecode_100ns, true);
		return {false, 0, 1.0f, 1.0f};
	}

	if (stream == Stream::Audio) {
		state_.last_audio_source_ns = source_ns;
		state_.last_audio_arrival_ns = arrival_ns;
		++audio_sequence_;
		if (state_.phase == AVGovernorPhase::Locked && state_.last_video_arrival_ns &&
			arrival_ns > state_.last_video_arrival_ns &&
			arrival_ns - state_.last_video_arrival_ns > video_stall_ns_) {
			const uint64_t output = adjusted_timestamp_locked(Stream::Audio, source_ns, arrival_ns);
			enter_hold_locked(AVGovernorReason::VideoStall, arrival_ns);
			++state_.fade_out_packets;
			record_locked(EventType::Fade, Stream::Audio, true, source_ns, arrival_ns, output,
				ndi_timestamp_100ns, ndi_timecode_100ns, true);
			return {true, output, 1.0f, 0.0f};
		}
	} else {
		update_video_interval_locked(source_ns);
		state_.last_video_source_ns = source_ns;
		state_.last_video_arrival_ns = arrival_ns;
		state_.video_stalled = false;
		++video_sequence_;
	}

	update_skew_locked();
	const bool just_locked = observe_pair_locked(arrival_ns);
	if (state_.phase != AVGovernorPhase::Locked) {
		if (stream == Stream::Audio)
			++state_.blocked_audio;
		else
			++state_.blocked_video;
		record_locked(EventType::Drop, stream, false, source_ns, arrival_ns, 0,
			ndi_timestamp_100ns, ndi_timecode_100ns, just_locked);
		return {false, 0, 1.0f, 1.0f};
	}

	if (magnitude(state_.baseline_deviation_ns) > max_deviation_ns_) {
		enter_hold_locked(AVGovernorReason::SkewExceeded, arrival_ns);
		if (stream == Stream::Audio)
			++state_.blocked_audio;
		else
			++state_.blocked_video;
		record_locked(EventType::Hold, stream, false, source_ns, arrival_ns, 0,
			ndi_timestamp_100ns, ndi_timecode_100ns, true);
		return {false, 0, 1.0f, 1.0f};
	}

	if (stream == Stream::Video)
		update_video_correction_locked();
	const uint64_t output_timestamp = adjusted_timestamp_locked(stream, source_ns, arrival_ns);
	if (!playout_depth_sane_locked(stream, output_timestamp, arrival_ns)) {
		enter_hold_locked(AVGovernorReason::PlayoutDepthExceeded, arrival_ns);
		if (stream == Stream::Audio)
			++state_.blocked_audio;
		else
			++state_.blocked_video;
		record_locked(EventType::Hold, stream, false, source_ns, arrival_ns, 0,
			ndi_timestamp_100ns, ndi_timecode_100ns, true);
		return {false, 0, 1.0f, 1.0f};
	}

	AVGovernorDecision decision{true, output_timestamp, 1.0f, 1.0f};
	if (stream == Stream::Audio && fade_in_pending_) {
		fade_in_pending_ = false;
		++state_.fade_in_packets;
		decision.audio_gain_start = 0.0f;
		decision.audio_gain_end = 1.0f;
		record_locked(EventType::Fade, stream, true, source_ns, arrival_ns, output_timestamp,
			ndi_timestamp_100ns, ndi_timecode_100ns, true);
	} else {
		record_locked(EventType::Sample, stream, true, source_ns, arrival_ns, output_timestamp,
			ndi_timestamp_100ns, ndi_timecode_100ns, false);
	}
	return decision;
}

AVGovernorDecision AVGovernor::process_audio(uint64_t source_ns, uint64_t arrival_ns,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return process_locked(Stream::Audio, source_ns, arrival_ns, ndi_timestamp_100ns, ndi_timecode_100ns);
}

AVGovernorDecision AVGovernor::process_video(uint64_t source_ns, uint64_t arrival_ns,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return process_locked(Stream::Video, source_ns, arrival_ns, ndi_timestamp_100ns, ndi_timecode_100ns);
}

AVGovernorSnapshot AVGovernor::snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto copy = state_;
	copy.recorder_events = static_cast<uint32_t>(event_count_);
	return copy;
}

void AVGovernor::record_locked(EventType type, Stream stream, bool accepted, uint64_t source_ns,
	uint64_t arrival_ns, uint64_t output_ns, int64_t ndi_timestamp_100ns,
	int64_t ndi_timecode_100ns, bool force)
{
	constexpr uint64_t sample_interval_ns = 100000000ULL;
	if (!force && type == EventType::Sample && last_record_sample_ns_ && arrival_ns >= last_record_sample_ns_ &&
		arrival_ns - last_record_sample_ns_ < sample_interval_ns)
		return;
	if (arrival_ns)
		last_record_sample_ns_ = arrival_ns;

	Event event;
	event.arrival_ns = arrival_ns;
	event.source_ns = source_ns;
	event.output_ns = output_ns;
	event.epoch = state_.epoch;
	event.audio_sequence = audio_sequence_;
	event.video_sequence = video_sequence_;
	event.raw_ndi_timestamp_100ns = ndi_timestamp_100ns;
	event.raw_ndi_timecode_100ns = ndi_timecode_100ns;
	event.raw_skew_ns = state_.raw_av_skew_ns;
	event.skew_ns = state_.av_skew_ns;
	event.deviation_ns = state_.baseline_deviation_ns;
	event.correction_ns = state_.video_correction_ns;
	event.rebase_ns = state_.epoch_rebase_ns;
	event.drift_ppm = state_.drift_ppm;
	event.drift_confidence = state_.drift_confidence;
	event.playout_depth_ns = stream == Stream::Audio
		? state_.audio_playout_depth_ns
		: state_.video_playout_depth_ns;
	event.phase = state_.phase;
	event.reason = state_.reason;
	event.type = type;
	event.stream = stream;
	event.accepted = accepted;
	events_[event_write_] = event;
	event_write_ = (event_write_ + 1) % kRecorderCapacity;
	event_count_ = std::min(event_count_ + 1, kRecorderCapacity);
}

std::string AVGovernor::flight_recorder_csv() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::ostringstream out;
	out << "arrival_ns,source_ns,output_ns,ndi_timestamp_100ns,ndi_timecode_100ns,epoch,"
		"audio_seq,video_seq,stream,event,accepted,phase,reason,raw_skew_ms,filtered_skew_ms,"
		"deviation_ms,drift_ppm,drift_confidence,playout_depth_ms,video_correction_ms,epoch_rebase_ms\n";
	const size_t begin = (event_write_ + kRecorderCapacity - event_count_) % kRecorderCapacity;
	for (size_t i = 0; i < event_count_; ++i) {
		const Event &event = events_[(begin + i) % kRecorderCapacity];
		out << event.arrival_ns << ',' << event.source_ns << ',' << event.output_ns << ','
			<< event.raw_ndi_timestamp_100ns << ',' << event.raw_ndi_timecode_100ns << ','
			<< event.epoch << ',' << event.audio_sequence << ',' << event.video_sequence << ','
			<< stream_name(event.stream) << ',' << event_name(event.type) << ','
			<< (event.accepted ? 1 : 0) << ',' << phase_name(event.phase) << ','
			<< reason_name(event.reason) << ','
			<< static_cast<double>(event.raw_skew_ns) / 1000000.0 << ','
			<< static_cast<double>(event.skew_ns) / 1000000.0 << ','
			<< static_cast<double>(event.deviation_ns) / 1000000.0 << ','
			<< event.drift_ppm << ',' << event.drift_confidence << ','
			<< static_cast<double>(event.playout_depth_ns) / 1000000.0 << ','
			<< static_cast<double>(event.correction_ns) / 1000000.0 << ','
			<< static_cast<double>(event.rebase_ns) / 1000000.0 << '\n';
	}
	return out.str();
}

} // namespace mcb
