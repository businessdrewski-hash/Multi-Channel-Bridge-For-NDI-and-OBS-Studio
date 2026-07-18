#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace mcb {

enum class AVGovernorPhase : uint8_t {
	Bypassed = 0,
	WarmingUp,
	Locked,
	Holding,
	Verifying,
	Failed,
};

enum class AVGovernorReason : uint8_t {
	None = 0,
	Startup,
	VideoStall,
	AudioDiscontinuity,
	VideoDiscontinuity,
	AudioNonMonotonic,
	VideoNonMonotonic,
	SkewExceeded,
	PlayoutDepthExceeded,
	SourceReconfigured,
	ManualReset,
	BaselineMismatch,
	RecoveryLimit,
};

struct AVGovernorDecision {
	bool accept = true;
	uint64_t output_timestamp_ns = 0;
	float audio_gain_start = 1.0f;
	float audio_gain_end = 1.0f;
};

struct AVGovernorSnapshot {
	bool enabled = false;
	bool source_configured = false;
	bool locked = false;
	bool video_stalled = false;
	bool baseline_valid = false;
	bool drift_correction_enabled = true;
	bool fail_safe_bypassed = false;
	bool correction_limited = false;
	bool verifying_trusted_baseline = false;
	AVGovernorPhase phase = AVGovernorPhase::Bypassed;
	AVGovernorReason reason = AVGovernorReason::None;
	int64_t raw_av_skew_ns = 0;
	int64_t av_skew_ns = 0;
	int64_t baseline_skew_ns = 0;
	int64_t candidate_baseline_skew_ns = 0;
	int64_t baseline_deviation_ns = 0;
	int64_t video_correction_ns = 0;
	int64_t target_video_correction_ns = 0;
	int64_t epoch_rebase_ns = 0;
	int64_t drift_ppm = 0;
	uint32_t drift_confidence = 0;
	uint64_t estimated_video_interval_ns = 0;
	int64_t audio_playout_depth_ns = 0;
	int64_t video_playout_depth_ns = 0;
	uint64_t playout_delay_ns = 0;
	uint64_t baseline_window_ns = 0;
	uint64_t drift_window_ns = 0;
	uint64_t drift_minimum_ns = 0;
	uint64_t audio_packets = 0;
	uint64_t video_frames = 0;
	uint64_t blocked_audio = 0;
	uint64_t blocked_video = 0;
	uint64_t discontinuities = 0;
	uint64_t lock_acquisitions = 0;
	uint64_t atomic_recoveries = 0;
	uint64_t failed_recoveries = 0;
	uint64_t quarantined_samples = 0;
	uint64_t correction_updates = 0;
	uint64_t monotonic_clamps = 0;
	uint64_t fade_out_packets = 0;
	uint64_t fade_in_packets = 0;
	uint64_t epoch = 0;
	uint32_t relock_progress = 0;
	uint32_t relock_required = 0;
	uint32_t baseline_samples = 0;
	uint32_t drift_samples = 0;
	uint32_t recorder_events = 0;
	uint64_t last_audio_arrival_ns = 0;
	uint64_t last_video_arrival_ns = 0;
	uint64_t last_audio_source_ns = 0;
	uint64_t last_video_source_ns = 0;
	uint64_t last_output_audio_ns = 0;
	uint64_t last_output_video_ns = 0;
	int64_t last_audio_ndi_timestamp_100ns = 0;
	int64_t last_audio_ndi_timecode_100ns = 0;
	int64_t last_video_ndi_timestamp_100ns = 0;
	int64_t last_video_ndi_timecode_100ns = 0;
};

// Receiver-side timeline governor. It relies on OBS's asynchronous source queue
// as the bounded playout buffer: audio and video are mapped onto one future
// timeline before either enters OBS. Audio samples remain untouched except for
// one short fade at a controlled recovery boundary. Slow correction is applied
// only to video, and only after a persistent drift trend is confirmed.
class AVGovernor {
public:
	void configure(bool enabled, int max_deviation_ms, int video_stall_ms,
		int playout_delay_ms, bool drift_correction, int max_video_correction_ms,
		int correction_slew_ppm, int relock_pairs, int baseline_window_ms = 5000,
		int drift_window_ms = 120000, int drift_minimum_ms = 30000,
		int drift_deadband_ppm = 8);
	void set_source_configured(bool configured);
	void reset(bool reset_counters);

	AVGovernorDecision process_audio(uint64_t source_ns, uint64_t arrival_ns,
		int64_t ndi_timestamp_100ns = 0, int64_t ndi_timecode_100ns = 0);
	AVGovernorDecision process_video(uint64_t source_ns, uint64_t arrival_ns,
		int64_t ndi_timestamp_100ns = 0, int64_t ndi_timecode_100ns = 0);
	AVGovernorSnapshot snapshot() const;
	std::string flight_recorder_csv() const;

private:
	enum class Stream : uint8_t { Audio = 0, Video = 1 };
	enum class EventType : uint8_t { Sample = 0, Hold, Lock, Correction, Drop, Reset, Fade, Failed };

	struct TrendSample {
		uint64_t arrival_ns = 0;
		int64_t deviation_ns = 0;
	};

	struct Event {
		uint64_t arrival_ns = 0;
		uint64_t source_ns = 0;
		uint64_t output_ns = 0;
		uint64_t epoch = 0;
		uint64_t audio_sequence = 0;
		uint64_t video_sequence = 0;
		int64_t raw_ndi_timestamp_100ns = 0;
		int64_t raw_ndi_timecode_100ns = 0;
		int64_t raw_skew_ns = 0;
		int64_t skew_ns = 0;
		int64_t trusted_baseline_ns = 0;
		int64_t candidate_baseline_ns = 0;
		int64_t deviation_ns = 0;
		int64_t correction_ns = 0;
		int64_t rebase_ns = 0;
		int64_t drift_ppm = 0;
		int64_t playout_depth_ns = 0;
		uint32_t drift_confidence = 0;
		AVGovernorPhase phase = AVGovernorPhase::Bypassed;
		AVGovernorReason reason = AVGovernorReason::None;
		EventType type = EventType::Sample;
		Stream stream = Stream::Audio;
		bool accepted = false;
		bool fail_safe_bypassed = false;
	};

	static constexpr size_t kCriticalRecorderCapacity = 128;
	static constexpr size_t kTelemetryRecorderCapacity = 896;
	static constexpr size_t kBaselineCapacity = 128;
	static constexpr size_t kSkewFilterCapacity = 9;
	static constexpr size_t kTrendCapacity = 512;
	static constexpr size_t kVideoIntervalCapacity = 17;
	static uint64_t ns_from_ms(int value);
	static uint64_t magnitude(int64_t value);
	static bool source_discontinuity(uint64_t previous, uint64_t current);
	static int64_t median(std::array<int64_t, kBaselineCapacity> values, size_t count);
	static uint64_t median_absolute_deviation(std::array<int64_t, kBaselineCapacity> values,
		size_t count, int64_t center);
	static int64_t median_small(std::array<int64_t, kSkewFilterCapacity> values, size_t count);
	static uint64_t median_intervals(std::array<uint64_t, kVideoIntervalCapacity> values, size_t count);
	static const char *phase_name(AVGovernorPhase phase);
	static const char *reason_name(AVGovernorReason reason);
	static const char *event_name(EventType type);
	static const char *stream_name(Stream stream);

	void reset_locked(bool reset_counters, AVGovernorReason reason, bool preserve_trusted_baseline = false);
	void enter_hold_locked(AVGovernorReason reason, uint64_t arrival_ns);
	void enter_failed_locked(AVGovernorReason reason, uint64_t arrival_ns);
	bool observe_pair_locked(uint64_t arrival_ns);
	void update_skew_locked();
	void push_skew_filter_locked(int64_t skew_ns);
	void update_trend_locked(uint64_t arrival_ns);
	void estimate_drift_locked();
	void update_video_interval_locked(uint64_t source_ns);
	void update_video_correction_locked();
	void establish_epoch_mapping_locked();
	uint64_t adjusted_timestamp_locked(Stream stream, uint64_t source_ns, uint64_t arrival_ns);
	bool playout_depth_sane_locked(Stream stream, uint64_t output_ns, uint64_t arrival_ns);
	AVGovernorDecision process_locked(Stream stream, uint64_t source_ns, uint64_t arrival_ns,
		int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns);
	void record_locked(EventType type, Stream stream, bool accepted, uint64_t source_ns,
		uint64_t arrival_ns, uint64_t output_ns, int64_t ndi_timestamp_100ns,
		int64_t ndi_timecode_100ns, bool force);

	mutable std::mutex mutex_;
	AVGovernorSnapshot state_{};
	uint64_t max_deviation_ns_ = 120000000ULL;
	uint64_t video_stall_ns_ = 120000000ULL;
	uint64_t max_video_correction_ns_ = 40000000ULL;
	uint64_t acquisition_limit_ns_ = 400000000ULL;
	uint64_t baseline_window_ns_ = 5000000000ULL;
	uint64_t drift_window_ns_ = 120000000000ULL;
	uint64_t drift_minimum_ns_ = 30000000000ULL;
	uint64_t trend_sample_interval_ns_ = 250000000ULL;
	uint64_t quarantine_ns_ = 2000000000ULL;
	uint64_t baseline_stability_limit_ns_ = 8000000ULL;
	uint64_t recovery_match_limit_ns_ = 40000000ULL;
	uint64_t recovery_limit_window_ns_ = 300000000000ULL;
	uint32_t correction_slew_ppm_ = 1000;
	uint32_t drift_deadband_ppm_ = 8;
	uint32_t relock_required_ = 3;
	uint64_t audio_sequence_ = 0;
	uint64_t video_sequence_ = 0;
	uint64_t paired_audio_sequence_ = 0;
	uint64_t paired_video_sequence_ = 0;
	uint64_t baseline_start_arrival_ns_ = 0;
	uint64_t quarantine_until_ns_ = 0;
	uint64_t stable_lock_start_ns_ = 0;
	uint64_t recovery_window_start_ns_ = 0;
	uint32_t recovery_attempts_ = 0;
	bool acquiring_initial_baseline_ = true;
	bool start_quarantine_on_next_pair_ = false;
	bool drift_confirmed_ = false;
	std::array<int64_t, kBaselineCapacity> baseline_samples_{};
	size_t baseline_count_ = 0;
	std::array<int64_t, kSkewFilterCapacity> skew_filter_{};
	size_t skew_filter_write_ = 0;
	size_t skew_filter_count_ = 0;
	std::array<TrendSample, kTrendCapacity> trend_{};
	size_t trend_write_ = 0;
	size_t trend_count_ = 0;
	uint64_t last_trend_sample_ns_ = 0;
	std::array<uint64_t, kVideoIntervalCapacity> video_intervals_{};
	size_t video_interval_write_ = 0;
	size_t video_interval_count_ = 0;
	uint64_t previous_video_for_interval_ns_ = 0;
	uint64_t last_output_audio_ns_ = 0;
	uint64_t last_output_video_ns_ = 0;
	uint64_t last_record_sample_ns_ = 0;
	uint64_t last_record_correction_ns_ = 0;
	int64_t last_recorded_correction_ns_ = 0;
	int64_t last_recorded_target_ns_ = 0;
	bool fade_in_pending_ = false;
	std::array<Event, kCriticalRecorderCapacity> critical_events_{};
	std::array<Event, kTelemetryRecorderCapacity> telemetry_events_{};
	size_t critical_write_ = 0;
	size_t critical_count_ = 0;
	size_t telemetry_write_ = 0;
	size_t telemetry_count_ = 0;
};

} // namespace mcb
