#include "downstream-sync-core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace mcb {

uint64_t LinkedAudioPacketClock::frames_to_ns(uint32_t frames, uint32_t sample_rate,
	uint64_t &remainder) noexcept
{
	if (!sample_rate)
		return 0;
	const uint64_t numerator = static_cast<uint64_t>(frames) * DownstreamSyncCore::kNsPerSecond + remainder;
	const uint64_t duration = numerator / sample_rate;
	remainder = numerator % sample_rate;
	return duration;
}

void LinkedAudioPacketClock::reset(uint64_t input_timestamp_ns, uint32_t sample_rate) noexcept
{
	sample_rate_ = sample_rate ? sample_rate : 48000;
	expected_input_timestamp_ns_ = input_timestamp_ns;
	next_output_timestamp_ns_ = input_timestamp_ns;
	input_timestamp_remainder_ = 0;
	output_timestamp_remainder_ = 0;
	frame_remainder_ = 0.0;
	net_frame_adjustment_ = 0;
	initialized_ = input_timestamp_ns != 0;
}

LinkedAudioPacketPlan LinkedAudioPacketClock::plan(uint32_t input_frames,
	uint64_t input_timestamp_ns, uint32_t sample_rate, double correction_ppm,
	bool enabled) noexcept
{
	if (!input_frames)
		return {};
	if (!enabled || !sample_rate || input_frames > kMaxInputFrames) {
		reset(input_timestamp_ns, sample_rate);
		return {input_frames, input_timestamp_ns, 0};
	}
	if (!initialized_ || sample_rate_ != sample_rate)
		reset(input_timestamp_ns, sample_rate);
	else if (input_timestamp_ns && expected_input_timestamp_ns_) {
		const int64_t timestamp_error = input_timestamp_ns >= expected_input_timestamp_ns_
			? static_cast<int64_t>(input_timestamp_ns - expected_input_timestamp_ns_)
			: -static_cast<int64_t>(expected_input_timestamp_ns_ - input_timestamp_ns);
		if (std::llabs(timestamp_error) > 50000000LL) {
			// Preserve accumulated correction for a raw timestamp re-anchor that
			// did not rebuild NDI. A full receiver restart calls reset() first.
			frame_remainder_ = 0.0;
			input_timestamp_remainder_ = 0;
			expected_input_timestamp_ns_ = input_timestamp_ns;
		}
	}

	const double ppm = std::clamp(correction_ppm, -5000.0, 5000.0);
	const double desired_frames = static_cast<double>(input_frames) *
		(1.0 + ppm * 1.0e-6) + frame_remainder_;
	const uint32_t output_frames = static_cast<uint32_t>(std::max(1.0, std::floor(desired_frames)));
	if (output_frames > kMaxOutputFrames) {
		reset(input_timestamp_ns, sample_rate);
		return {input_frames, input_timestamp_ns, 0};
	}
	frame_remainder_ = desired_frames - static_cast<double>(output_frames);
	const uint64_t output_timestamp = next_output_timestamp_ns_
		? next_output_timestamp_ns_ : input_timestamp_ns;
	if (input_timestamp_ns)
		expected_input_timestamp_ns_ = input_timestamp_ns +
			frames_to_ns(input_frames, sample_rate_, input_timestamp_remainder_);
	if (output_timestamp)
		next_output_timestamp_ns_ = output_timestamp +
			frames_to_ns(output_frames, sample_rate_, output_timestamp_remainder_);
	net_frame_adjustment_ += static_cast<int64_t>(output_frames) -
		static_cast<int64_t>(input_frames);
	return {output_frames, output_timestamp, net_frame_adjustment_};
}

uint64_t DownstreamSyncCore::magnitude(int64_t value) noexcept
{
	if (value >= 0)
		return static_cast<uint64_t>(value);
	return static_cast<uint64_t>(-(value + 1)) + 1ULL;
}

int64_t DownstreamSyncCore::signed_delta(uint64_t a, uint64_t b) noexcept
{
	if (a >= b) {
		const uint64_t delta = a - b;
		return delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
			? std::numeric_limits<int64_t>::max()
			: static_cast<int64_t>(delta);
	}
	const uint64_t delta = b - a;
	return delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
		? std::numeric_limits<int64_t>::min()
		: -static_cast<int64_t>(delta);
}

int64_t DownstreamSyncCore::median_small(
	std::array<int64_t, kFilterCapacity> values, size_t count)
{
	if (!count)
		return 0;
	count = std::min(count, values.size());
	std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
	return values[count / 2];
}

int64_t DownstreamSyncCore::median_baseline(
	std::array<int64_t, kBaselineCapacity> values, size_t count)
{
	if (!count)
		return 0;
	count = std::min(count, values.size());
	std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
	return values[count / 2];
}

uint64_t DownstreamSyncCore::median_absolute_deviation(
	std::array<int64_t, kBaselineCapacity> values, size_t count, int64_t center)
{
	count = std::min(count, values.size());
	for (size_t i = 0; i < count; ++i)
		values[i] = static_cast<int64_t>(magnitude(values[i] - center));
	return static_cast<uint64_t>(median_baseline(values, count));
}

void DownstreamSyncCore::configure(bool enabled, int max_correction_ppm,
	int slew_ppm_per_second, int dead_zone_ms, int baseline_window_ms,
	int drift_window_ms, int drift_minimum_ms)
{
	enabled_.store(enabled, std::memory_order_relaxed);
	max_correction_ppm_.store(std::clamp(max_correction_ppm, 25, 5000));
	slew_ppm_per_second_.store(std::clamp(slew_ppm_per_second, 1, 1000));
	dead_zone_ms_.store(std::clamp(dead_zone_ms, 1, 50));
	baseline_window_ns_.store(static_cast<uint64_t>(std::clamp(baseline_window_ms, 5000, 30000)) * 1000000ULL);
	const int bounded_window = std::clamp(drift_window_ms, 30000, 300000);
	drift_window_ns_.store(static_cast<uint64_t>(bounded_window) * 1000000ULL);
	drift_minimum_ns_.store(static_cast<uint64_t>(std::clamp(drift_minimum_ms, 30000, bounded_window)) * 1000000ULL);
	reset(false, false);
}

void DownstreamSyncCore::clear_learning_locked(bool preserve_baseline)
{
	const bool had_baseline = preserve_baseline && state_.baseline_valid;
	const int64_t baseline = had_baseline ? state_.baseline_ns : 0;
	state_ = {};
	state_.enabled = enabled_.load(std::memory_order_relaxed);
	state_.baseline_valid = had_baseline;
	state_.baseline_ns = baseline;
	state_.phase = !state_.enabled ? DownstreamSyncPhase::Bypassed
		: had_baseline ? DownstreamSyncPhase::Verifying : DownstreamSyncPhase::Learning;
	state_.sample_rate = sample_rate_.load(std::memory_order_relaxed);
	baseline_start_ns_ = 0;
	last_tick_ns_ = 0;
	last_trend_sample_ns_ = 0;
	filter_write_ = 0;
	filter_count_ = 0;
	baseline_count_ = 0;
	trend_write_ = 0;
	trend_count_ = 0;
}

void DownstreamSyncCore::reset(bool reset_counters, bool preserve_trusted_baseline)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const uint64_t discontinuities = reset_counters ? 0 : state_.discontinuities;
	const uint64_t quarantined = reset_counters ? 0 : state_.quarantined_samples;
	clear_learning_locked(preserve_trusted_baseline);
	state_.discontinuities = discontinuities;
	state_.quarantined_samples = quarantined;
	correction_ppm_.store(0.0, std::memory_order_relaxed);
	target_ppm_.store(0.0, std::memory_order_relaxed);
	net_frame_adjustment_.store(0, std::memory_order_relaxed);
	corrected_blocks_.store(0, std::memory_order_relaxed);
	// A receiver reconnect starts a new media-timestamp epoch. Do not allow a
	// fresh learning/verification window to consume the last pre-reconnect
	// video, audio, or corrected-output observation while the NDI receiver is
	// being rebuilt asynchronously.
	video_timestamp_ns_.store(0, std::memory_order_relaxed);
	video_wall_ns_.store(0, std::memory_order_relaxed);
	audio_timestamp_ns_.store(0, std::memory_order_relaxed);
	audio_wall_ns_.store(0, std::memory_order_relaxed);
	output_timestamp_ns_.store(0, std::memory_order_relaxed);
	output_wall_ns_.store(0, std::memory_order_relaxed);
	observed_incident_generation_ = incident_generation_.load(std::memory_order_acquire);
	last_incident_wall_ns_.store(0, std::memory_order_relaxed);
	quarantine_until_ns_ = 0;
}

void DownstreamSyncCore::observe_clock(std::atomic<uint64_t> &timestamp,
	std::atomic<uint64_t> &wall, std::atomic<uint64_t> &observations,
	uint64_t timestamp_ns, uint64_t wall_ns) noexcept
{
	if (!timestamp_ns || !wall_ns)
		return;
	const uint64_t previous_timestamp = timestamp.exchange(timestamp_ns, std::memory_order_acq_rel);
	const uint64_t previous_wall = wall.exchange(wall_ns, std::memory_order_acq_rel);
	observations.fetch_add(1, std::memory_order_relaxed);
	if (!previous_timestamp || !previous_wall || wall_ns <= previous_wall)
		return;
	const int64_t timestamp_delta = signed_delta(timestamp_ns, previous_timestamp);
	const int64_t wall_delta = signed_delta(wall_ns, previous_wall);
	const int64_t error = timestamp_delta - wall_delta;
	if (magnitude(error) <= 50000000ULL)
		return;
	const uint64_t prior_incident = last_incident_wall_ns_.load(std::memory_order_relaxed);
	if (prior_incident && wall_ns > prior_incident && wall_ns - prior_incident < kNsPerSecond)
		return;
	last_incident_wall_ns_.store(wall_ns, std::memory_order_relaxed);
	incident_generation_.fetch_add(1, std::memory_order_acq_rel);
}

void DownstreamSyncCore::observe_video(uint64_t timestamp_ns, uint64_t wall_ns) noexcept
{
	observe_clock(video_timestamp_ns_, video_wall_ns_, video_observations_, timestamp_ns, wall_ns);
}

void DownstreamSyncCore::observe_audio_input(uint64_t timestamp_ns, uint64_t wall_ns) noexcept
{
	observe_clock(audio_timestamp_ns_, audio_wall_ns_, audio_observations_, timestamp_ns, wall_ns);
}

void DownstreamSyncCore::report_audio_output(uint64_t timestamp_ns, uint64_t wall_ns,
	int64_t net_frame_adjustment, uint32_t sample_rate) noexcept
{
	output_timestamp_ns_.store(timestamp_ns, std::memory_order_relaxed);
	output_wall_ns_.store(wall_ns, std::memory_order_relaxed);
	net_frame_adjustment_.store(net_frame_adjustment, std::memory_order_relaxed);
	corrected_blocks_.fetch_add(1, std::memory_order_relaxed);
	if (sample_rate)
		sample_rate_.store(sample_rate, std::memory_order_relaxed);
}

void DownstreamSyncCore::handle_incident_locked(uint64_t wall_ns)
{
	++state_.discontinuities;
	state_.phase = state_.baseline_valid ? DownstreamSyncPhase::Verifying : DownstreamSyncPhase::Learning;
	quarantine_until_ns_ = wall_ns + 2000000000ULL;
	baseline_start_ns_ = 0;
	baseline_count_ = 0;
	filter_count_ = 0;
	filter_write_ = 0;
	trend_count_ = 0;
	trend_write_ = 0;
	last_trend_sample_ns_ = 0;
	correction_ppm_.store(0.0, std::memory_order_relaxed);
	target_ppm_.store(0.0, std::memory_order_relaxed);
}

void DownstreamSyncCore::push_filtered_locked(int64_t raw_relation_ns)
{
	filter_[filter_write_] = raw_relation_ns;
	filter_write_ = (filter_write_ + 1) % filter_.size();
	filter_count_ = std::min(filter_count_ + 1, filter_.size());
	std::array<int64_t, kFilterCapacity> raw_copy{};
	for (size_t i = 0; i < filter_count_; ++i)
		raw_copy[i] = filter_[i];
	const int64_t filtered_raw = median_small(raw_copy, filter_count_);
	const uint32_t rate = std::max<uint32_t>(1, sample_rate_.load(std::memory_order_relaxed));
	const long double trim_ns = static_cast<long double>(net_frame_adjustment_.load(std::memory_order_relaxed)) *
		static_cast<long double>(kNsPerSecond) / static_cast<long double>(rate);
	state_.relation_ns = filtered_raw + static_cast<int64_t>(std::llround(trim_ns));
	if (state_.baseline_valid) {
		state_.raw_deviation_ns = filtered_raw - state_.baseline_ns;
		state_.corrected_deviation_ns = state_.relation_ns - state_.baseline_ns;
	}
}

void DownstreamSyncCore::push_trend_locked(uint64_t wall_ns)
{
	if (last_trend_sample_ns_ && wall_ns - last_trend_sample_ns_ < 250000000ULL)
		return;
	last_trend_sample_ns_ = wall_ns;
	trend_[trend_write_] = {wall_ns, state_.raw_deviation_ns};
	trend_write_ = (trend_write_ + 1) % trend_.size();
	trend_count_ = std::min(trend_count_ + 1, trend_.size());
	estimate_drift_locked();
}

void DownstreamSyncCore::estimate_drift_locked()
{
	const uint64_t window = drift_window_ns_.load(std::memory_order_relaxed);
	uint64_t newest = 0;
	for (size_t i = 0; i < trend_count_; ++i)
		newest = std::max(newest, trend_[i].wall_ns);
	const uint64_t oldest_allowed = newest > window ? newest - window : 0;
	std::array<TrendSample, kTrendCapacity> samples{};
	size_t count = 0;
	for (size_t i = 0; i < trend_count_; ++i) {
		if (trend_[i].wall_ns >= oldest_allowed)
			samples[count++] = trend_[i];
	}
	state_.drift_samples = static_cast<uint32_t>(count);
	if (count < 3) {
		state_.drift_ppm = 0;
		state_.confidence = 0;
		return;
	}
	uint64_t first = samples[0].wall_ns;
	uint64_t last = samples[0].wall_ns;
	for (size_t i = 1; i < count; ++i) {
		first = std::min(first, samples[i].wall_ns);
		last = std::max(last, samples[i].wall_ns);
	}
	const uint64_t span = last - first;
	if (!span)
		return;
	long double sum_t = 0.0L, sum_y = 0.0L, sum_tt = 0.0L, sum_ty = 0.0L;
	for (size_t i = 0; i < count; ++i) {
		const long double t = static_cast<long double>(samples[i].wall_ns - first);
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
	long double residual = 0.0L;
	for (size_t i = 0; i < count; ++i) {
		const long double t = static_cast<long double>(samples[i].wall_ns - first);
		residual += std::fabs(static_cast<long double>(samples[i].deviation_ns) - (intercept + slope * t));
	}
	const long double ppm = std::clamp(slope * 1000000.0L, -100000.0L, 100000.0L);
	state_.drift_ppm = static_cast<int64_t>(std::llround(ppm));
	const long double span_score = std::min<long double>(1.0L,
		static_cast<long double>(span) / static_cast<long double>(drift_minimum_ns_.load(std::memory_order_relaxed)));
	const long double sample_score = std::min<long double>(1.0L, static_cast<long double>(count) / 24.0L);
	const long double residual_score = std::clamp(1.0L - (residual / n) / 5000000.0L, 0.0L, 1.0L);
	state_.confidence = static_cast<uint32_t>(std::clamp<long double>(100.0L * span_score * sample_score * residual_score, 0.0L, 100.0L));
}

void DownstreamSyncCore::update_controller_locked(uint64_t wall_ns)
{
	double current = correction_ppm_.load(std::memory_order_relaxed);
	double target = current;
	const int max_ppm = max_correction_ppm_.load(std::memory_order_relaxed);
	const double dead_zone_ns = static_cast<double>(dead_zone_ms_.load(std::memory_order_relaxed)) * 1000000.0;
	const bool reliable = state_.confidence >= 75;
	if (reliable) {
		// The raw downstream slope is measured before our filters, so it is the
		// native clock mismatch. Positive video-minus-audio drift means audio is
		// falling behind and needs negative ppm (slightly fewer output samples).
		state_.native_audio_error_ppm = state_.drift_ppm;
		target = -static_cast<double>(state_.native_audio_error_ppm);
		const double error = static_cast<double>(state_.corrected_deviation_ns);
		if (std::fabs(error) > dead_zone_ns) {
			const double outside_ms = (std::fabs(error) - dead_zone_ns) / 1000000.0;
			const double normalized = std::clamp(outside_ms / 250.0, 0.0, 1.0);
			const double catchup = static_cast<double>(max_ppm) * normalized * std::sqrt(normalized);
			target += -std::copysign(catchup, error);
		}
	} else {
		target = 0.0;
	}
	target = std::clamp(target, -static_cast<double>(max_ppm), static_cast<double>(max_ppm));
	state_.correction_limited = reliable && std::fabs(target) >= static_cast<double>(max_ppm) * 0.99;
	target_ppm_.store(target, std::memory_order_relaxed);
	const double dt = last_tick_ns_ && wall_ns > last_tick_ns_
		? std::clamp(static_cast<double>(wall_ns - last_tick_ns_) / static_cast<double>(kNsPerSecond), 0.01, 2.0)
		: 0.25;
	const double slew = static_cast<double>(slew_ppm_per_second_.load(std::memory_order_relaxed));
	const bool braking = std::fabs(target) < std::fabs(current) || target * current < 0.0;
	const double step = slew * (braking ? 2.0 : 1.0) * dt;
	current += std::clamp(target - current, -step, step);
	correction_ppm_.store(current, std::memory_order_relaxed);
	state_.correction_active = std::fabs(current) >= 0.25;
	state_.correction_ppm = current;
	state_.target_ppm = target;
}

void DownstreamSyncCore::tick(uint64_t wall_ns)
{
	std::lock_guard<std::mutex> lock(mutex_);
	state_.enabled = enabled_.load(std::memory_order_relaxed);
	if (!state_.enabled) {
		state_.phase = DownstreamSyncPhase::Bypassed;
		correction_ppm_.store(0.0, std::memory_order_relaxed);
		target_ppm_.store(0.0, std::memory_order_relaxed);
		return;
	}
	const uint64_t incident = incident_generation_.load(std::memory_order_acquire);
	if (incident != observed_incident_generation_) {
		observed_incident_generation_ = incident;
		handle_incident_locked(wall_ns);
	}
	const uint64_t video_ts = video_timestamp_ns_.load(std::memory_order_acquire);
	const uint64_t video_wall = video_wall_ns_.load(std::memory_order_acquire);
	const uint64_t audio_ts = audio_timestamp_ns_.load(std::memory_order_acquire);
	const uint64_t audio_wall = audio_wall_ns_.load(std::memory_order_acquire);
	const uint64_t output_ts = output_timestamp_ns_.load(std::memory_order_acquire);
	const uint64_t output_wall = output_wall_ns_.load(std::memory_order_acquire);
	state_.video_observations = video_observations_.load(std::memory_order_relaxed);
	state_.audio_observations = audio_observations_.load(std::memory_order_relaxed);
	state_.last_video_wall_ns = video_wall;
	state_.last_audio_wall_ns = audio_wall;
	state_.sample_rate = sample_rate_.load(std::memory_order_relaxed);
	state_.corrected_blocks = corrected_blocks_.load(std::memory_order_relaxed);
	state_.net_frame_adjustment = net_frame_adjustment_.load(std::memory_order_relaxed);
	state_.measurement_fresh = video_ts && audio_ts && output_ts && wall_ns >= video_wall &&
		wall_ns >= audio_wall && wall_ns >= output_wall && wall_ns - video_wall < 500000000ULL &&
		wall_ns - audio_wall < 500000000ULL && wall_ns - output_wall < 500000000ULL;
	if (!state_.measurement_fresh || wall_ns < quarantine_until_ns_) {
		if (wall_ns < quarantine_until_ns_)
			++state_.quarantined_samples;
		correction_ppm_.store(0.0, std::memory_order_relaxed);
		target_ppm_.store(0.0, std::memory_order_relaxed);
		state_.correction_active = false;
		last_tick_ns_ = wall_ns;
		return;
	}
	const int64_t video_projected = signed_delta(video_ts, 0) + signed_delta(wall_ns, video_wall);
	const int64_t audio_projected = signed_delta(audio_ts, 0) + signed_delta(wall_ns, audio_wall);
	push_filtered_locked(video_projected - audio_projected);

	if (state_.phase != DownstreamSyncPhase::Locked) {
		if (!baseline_start_ns_)
			baseline_start_ns_ = wall_ns;
		if (baseline_count_ < baseline_samples_.size())
			baseline_samples_[baseline_count_++] = state_.relation_ns;
		else {
			std::move(baseline_samples_.begin() + 1, baseline_samples_.end(), baseline_samples_.begin());
			baseline_samples_.back() = state_.relation_ns;
		}
		state_.baseline_samples = static_cast<uint32_t>(baseline_count_);
		if (baseline_count_ >= 12 && wall_ns - baseline_start_ns_ >= baseline_window_ns_.load(std::memory_order_relaxed)) {
			const int64_t candidate = median_baseline(baseline_samples_, baseline_count_);
			const uint64_t instability = median_absolute_deviation(baseline_samples_, baseline_count_, candidate);
			if (instability <= 8000000ULL) {
				if (state_.baseline_valid && magnitude(candidate - state_.baseline_ns) > 40000000ULL) {
					state_.phase = DownstreamSyncPhase::Failed;
					correction_ppm_.store(0.0, std::memory_order_relaxed);
					target_ppm_.store(0.0, std::memory_order_relaxed);
					return;
				}
				if (!state_.baseline_valid) {
					state_.baseline_ns = candidate;
					state_.baseline_valid = true;
				}
				state_.phase = DownstreamSyncPhase::Locked;
				state_.raw_deviation_ns = state_.relation_ns - state_.baseline_ns;
				state_.corrected_deviation_ns = state_.raw_deviation_ns;
				trend_count_ = 0;
				trend_write_ = 0;
				last_trend_sample_ns_ = 0;
			} else {
				baseline_count_ = 0;
				baseline_start_ns_ = wall_ns;
			}
		}
		last_tick_ns_ = wall_ns;
		return;
	}

	push_trend_locked(wall_ns);
	update_controller_locked(wall_ns);
	last_tick_ns_ = wall_ns;
}

DownstreamSyncSnapshot DownstreamSyncCore::snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto copy = state_;
	copy.correction_ppm = correction_ppm_.load(std::memory_order_relaxed);
	copy.target_ppm = target_ppm_.load(std::memory_order_relaxed);
	copy.net_frame_adjustment = net_frame_adjustment_.load(std::memory_order_relaxed);
	copy.corrected_blocks = corrected_blocks_.load(std::memory_order_relaxed);
	return copy;
}

std::string DownstreamSyncCore::diagnostics_csv() const
{
	const auto s = snapshot();
	std::ostringstream out;
	out << "measurement,phase,relation_ms,trusted_ms,raw_deviation_ms,corrected_deviation_ms,"
		"drift_ppm,confidence,applied_audio_ppm,target_audio_ppm,net_frame_adjustment\n";
	out << "downstream," << static_cast<int>(s.phase) << ','
		<< static_cast<double>(s.relation_ns) / 1e6 << ','
		<< static_cast<double>(s.baseline_ns) / 1e6 << ','
		<< static_cast<double>(s.raw_deviation_ns) / 1e6 << ','
		<< static_cast<double>(s.corrected_deviation_ns) / 1e6 << ','
		<< s.drift_ppm << ',' << s.confidence << ',' << s.correction_ppm << ','
		<< s.target_ppm << ',' << s.net_frame_adjustment << '\n';
	return out.str();
}

} // namespace mcb
