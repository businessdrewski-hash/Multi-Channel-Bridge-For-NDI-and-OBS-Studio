#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace mcb {

enum class DownstreamSyncPhase : uint8_t {
	Bypassed = 0,
	Learning,
	Locked,
	Verifying,
	Failed,
};

struct DownstreamSyncSnapshot {
	bool enabled = false;
	bool baseline_valid = false;
	bool correction_active = false;
	bool correction_limited = false;
	bool measurement_fresh = false;
	DownstreamSyncPhase phase = DownstreamSyncPhase::Bypassed;
	int64_t relation_ns = 0;          // video minus raw audio, projected to one wall-clock instant
	int64_t baseline_ns = 0;
	int64_t raw_deviation_ns = 0;
	int64_t corrected_deviation_ns = 0;
	int64_t drift_ppm = 0;
	int64_t native_audio_error_ppm = 0;
	double correction_ppm = 0.0;
	double target_ppm = 0.0;
	uint32_t confidence = 0;
	uint32_t baseline_samples = 0;
	uint32_t drift_samples = 0;
	uint64_t video_observations = 0;
	uint64_t audio_observations = 0;
	uint64_t discontinuities = 0;
	uint64_t quarantined_samples = 0;
	uint64_t corrected_blocks = 0;
	int64_t net_frame_adjustment = 0;
	uint32_t sample_rate = 48000;
	uint64_t last_video_wall_ns = 0;
	uint64_t last_audio_wall_ns = 0;
};

struct LinkedAudioPacketPlan {
	uint32_t output_frames = 0;
	uint64_t output_timestamp_ns = 0;
	int64_t net_frame_adjustment = 0;
};

// Allocation-free actuator clock for the shared four-channel source packet.
// One instance owns the fractional frame remainder and corrected timestamp so
// both stereo proxies are guaranteed to receive the same packet duration.
class LinkedAudioPacketClock {
public:
	static constexpr uint32_t kMaxInputFrames = 4096;
	static constexpr uint32_t kMaxOutputFrames = 4128;

	void reset(uint64_t input_timestamp_ns, uint32_t sample_rate) noexcept;
	LinkedAudioPacketPlan plan(uint32_t input_frames, uint64_t input_timestamp_ns,
		uint32_t sample_rate, double correction_ppm, bool enabled) noexcept;

private:
	static uint64_t frames_to_ns(uint32_t frames, uint32_t sample_rate,
		uint64_t &remainder) noexcept;
	uint32_t sample_rate_ = 48000;
	uint64_t expected_input_timestamp_ns_ = 0;
	uint64_t next_output_timestamp_ns_ = 0;
	uint64_t input_timestamp_remainder_ = 0;
	uint64_t output_timestamp_remainder_ = 0;
	double frame_remainder_ = 0.0;
	int64_t net_frame_adjustment_ = 0;
	bool initialized_ = false;
};

// Receiver-side controller modeled after Sync Guardian's proven observation
// point. Video is observed by an OBS async-video filter; raw audio is observed
// at the shared four-channel receiver handoff before one linked correction is
// submitted to the two proxy sources. Callbacks only publish atomics; trend
// analysis and UI snapshots never wait on those callbacks.
class DownstreamSyncCore {
public:
	static constexpr uint64_t kNsPerSecond = 1000000000ULL;

	void configure(bool enabled, int max_correction_ppm = 1000,
		int slew_ppm_per_second = 100, int dead_zone_ms = 4,
		int baseline_window_ms = 5000, int drift_window_ms = 120000,
		int drift_minimum_ms = 30000);
	void reset(bool reset_counters, bool preserve_trusted_baseline = false);

	void observe_video(uint64_t timestamp_ns, uint64_t wall_ns) noexcept;
	void observe_audio_input(uint64_t timestamp_ns, uint64_t wall_ns) noexcept;
	void report_audio_output(uint64_t timestamp_ns, uint64_t wall_ns,
		int64_t net_frame_adjustment, uint32_t sample_rate) noexcept;

	// Called from a 250 ms control timer, never from an OBS media callback.
	void tick(uint64_t wall_ns);

	double correction_ppm() const noexcept
	{
		return correction_ppm_.load(std::memory_order_relaxed);
	}
	bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

	DownstreamSyncSnapshot snapshot() const;
	std::string diagnostics_csv() const;

private:
	struct TrendSample {
		uint64_t wall_ns = 0;
		int64_t deviation_ns = 0;
	};

	static constexpr size_t kFilterCapacity = 9;
	static constexpr size_t kBaselineCapacity = 128;
	static constexpr size_t kTrendCapacity = 512;
	static uint64_t magnitude(int64_t value) noexcept;
	static int64_t signed_delta(uint64_t a, uint64_t b) noexcept;
	static int64_t median_small(std::array<int64_t, kFilterCapacity> values, size_t count);
	static int64_t median_baseline(std::array<int64_t, kBaselineCapacity> values, size_t count);
	static uint64_t median_absolute_deviation(
		std::array<int64_t, kBaselineCapacity> values, size_t count, int64_t center);
	void observe_clock(std::atomic<uint64_t> &timestamp, std::atomic<uint64_t> &wall,
		std::atomic<uint64_t> &observations, uint64_t timestamp_ns, uint64_t wall_ns) noexcept;
	void handle_incident_locked(uint64_t wall_ns);
	void push_filtered_locked(int64_t raw_relation_ns);
	void push_trend_locked(uint64_t wall_ns);
	void estimate_drift_locked();
	void update_controller_locked(uint64_t wall_ns);
	void clear_learning_locked(bool preserve_baseline);

	std::atomic_bool enabled_{false};
	std::atomic<int> max_correction_ppm_{1000};
	std::atomic<int> slew_ppm_per_second_{100};
	std::atomic<int> dead_zone_ms_{4};
	std::atomic<uint64_t> baseline_window_ns_{5000000000ULL};
	std::atomic<uint64_t> drift_window_ns_{120000000000ULL};
	std::atomic<uint64_t> drift_minimum_ns_{30000000000ULL};
	std::atomic<double> correction_ppm_{0.0};
	std::atomic<double> target_ppm_{0.0};
	std::atomic<uint64_t> video_timestamp_ns_{0};
	std::atomic<uint64_t> video_wall_ns_{0};
	std::atomic<uint64_t> audio_timestamp_ns_{0};
	std::atomic<uint64_t> audio_wall_ns_{0};
	std::atomic<uint64_t> video_observations_{0};
	std::atomic<uint64_t> audio_observations_{0};
	std::atomic<uint64_t> incident_generation_{0};
	std::atomic<uint64_t> last_incident_wall_ns_{0};
	std::atomic<uint64_t> output_timestamp_ns_{0};
	std::atomic<uint64_t> output_wall_ns_{0};
	std::atomic<int64_t> net_frame_adjustment_{0};
	std::atomic<uint64_t> corrected_blocks_{0};
	std::atomic<uint32_t> sample_rate_{48000};

	mutable std::mutex mutex_;
	DownstreamSyncSnapshot state_{};
	uint64_t observed_incident_generation_ = 0;
	uint64_t quarantine_until_ns_ = 0;
	uint64_t baseline_start_ns_ = 0;
	uint64_t last_tick_ns_ = 0;
	uint64_t last_trend_sample_ns_ = 0;
	std::array<int64_t, kFilterCapacity> filter_{};
	size_t filter_write_ = 0;
	size_t filter_count_ = 0;
	std::array<int64_t, kBaselineCapacity> baseline_samples_{};
	size_t baseline_count_ = 0;
	std::array<TrendSample, kTrendCapacity> trend_{};
	size_t trend_write_ = 0;
	size_t trend_count_ = 0;
};

} // namespace mcb
