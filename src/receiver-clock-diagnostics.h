#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace distroav::clocklab {

enum class Event : uint8_t {
	None = 0,
	LoggingEnabled,
	ReceiverReset,
	ReceiverReady,
	ModeChanged,
	DiagnosticsChanged,
};

struct StageSnapshot {
	int64_t ndi_timestamp_100ns = 0;
	int64_t ndi_timecode_100ns = 0;
	uint64_t timestamp_ns = 0;
	uint64_t wall_ns = 0;
	int64_t timestamp_delta_ns = 0;
	int64_t wall_delta_ns = 0;
	uint32_t unit_a = 0;
	uint32_t unit_b = 0;
	uint32_t unit_c = 0;
	uint64_t observations = 0;
	uint64_t pacing_anomalies = 0;
};

struct SchedulerSnapshot {
	int32_t mode = 0;
	uint64_t receiver_epoch_ns = 0;
	uint64_t next_audio_deadline_ns = 0;
	uint64_t next_video_deadline_ns = 0;
	uint64_t cumulative_audio_frames = 0;
	uint64_t video_ticks = 0;
	int64_t audio_deadline_error_ns = 0;
	int64_t video_deadline_error_ns = 0;
	uint64_t audio_catchups = 0;
	uint64_t video_catchups = 0;
	uint64_t repeated_video_frames = 0;
	uint64_t empty_audio_pulls = 0;
	uint64_t empty_video_pulls = 0;
	int64_t ndi_total_audio_frames = 0;
	int64_t ndi_total_video_frames = 0;
	int64_t ndi_dropped_audio_frames = 0;
	int64_t ndi_dropped_video_frames = 0;
	int32_t ndi_queued_audio_frames = 0;
	int32_t ndi_queued_video_frames = 0;
};

// Callback-facing observations only publish atomics. A compact 250 ms flight
// recorder is allocated only when the source's diagnostics checkbox is enabled.
class Diagnostics {
public:
	static constexpr size_t kCapacity = 43200; // three hours at 250 ms

	void set_enabled(bool enabled, uint64_t wall_ns);
	void set_live_output_path(std::string path);
	bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
	void mark_event(Event event, uint64_t wall_ns) noexcept;

	void observe_capture_audio(int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns, uint64_t timestamp_ns,
				   uint64_t wall_ns, uint32_t frames, uint32_t sample_rate, uint32_t channels) noexcept;
	void observe_capture_video(int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns, uint64_t timestamp_ns,
				   uint64_t wall_ns, uint32_t width, uint32_t height) noexcept;
	void observe_output_audio(uint64_t timestamp_ns, uint64_t wall_ns, uint32_t frames, uint32_t sample_rate,
				  uint32_t channels) noexcept;
	void observe_output_video(uint64_t timestamp_ns, uint64_t wall_ns, uint32_t width, uint32_t height) noexcept;
	void observe_filtered_audio(uint64_t timestamp_ns, uint64_t wall_ns, uint32_t frames) noexcept;
	void observe_selected_video(uint64_t timestamp_ns, uint64_t wall_ns) noexcept;
	void update_scheduler(const SchedulerSnapshot &snapshot) noexcept;
	void sample(uint64_t wall_ns);

	std::string csv() const;
	size_t sample_count() const;
	uint64_t overwritten_samples() const;

private:
	struct StageAtomics {
		std::atomic<uint64_t> sequence{0};
		std::atomic<int64_t> ndi_timestamp_100ns{0};
		std::atomic<int64_t> ndi_timecode_100ns{0};
		std::atomic<uint64_t> timestamp_ns{0};
		std::atomic<uint64_t> wall_ns{0};
		std::atomic<int64_t> timestamp_delta_ns{0};
		std::atomic<int64_t> wall_delta_ns{0};
		std::atomic<uint32_t> unit_a{0};
		std::atomic<uint32_t> unit_b{0};
		std::atomic<uint32_t> unit_c{0};
		std::atomic<uint64_t> observations{0};
		std::atomic<uint64_t> pacing_anomalies{0};
	};

	struct SchedulerAtomics {
		std::atomic<int32_t> mode{0};
		std::atomic<uint64_t> receiver_epoch_ns{0};
		std::atomic<uint64_t> next_audio_deadline_ns{0};
		std::atomic<uint64_t> next_video_deadline_ns{0};
		std::atomic<uint64_t> cumulative_audio_frames{0};
		std::atomic<uint64_t> video_ticks{0};
		std::atomic<int64_t> audio_deadline_error_ns{0};
		std::atomic<int64_t> video_deadline_error_ns{0};
		std::atomic<uint64_t> audio_catchups{0};
		std::atomic<uint64_t> video_catchups{0};
		std::atomic<uint64_t> repeated_video_frames{0};
		std::atomic<uint64_t> empty_audio_pulls{0};
		std::atomic<uint64_t> empty_video_pulls{0};
		std::atomic<int64_t> ndi_total_audio_frames{0};
		std::atomic<int64_t> ndi_total_video_frames{0};
		std::atomic<int64_t> ndi_dropped_audio_frames{0};
		std::atomic<int64_t> ndi_dropped_video_frames{0};
		std::atomic<int32_t> ndi_queued_audio_frames{0};
		std::atomic<int32_t> ndi_queued_video_frames{0};
	};

	struct Sample {
		uint64_t wall_ns = 0;
		uint64_t session = 0;
		Event event = Event::None;
		uint64_t event_generation = 0;
		uint64_t event_wall_ns = 0;
		uint64_t overwritten_at_capture = 0;
		StageSnapshot capture_audio;
		StageSnapshot capture_video;
		StageSnapshot output_audio;
		StageSnapshot output_video;
		StageSnapshot filtered_audio;
		StageSnapshot selected_video;
		SchedulerSnapshot scheduler;
	};

	static int64_t signed_delta(uint64_t current, uint64_t previous) noexcept;
	static const char *event_name(Event event) noexcept;
	static void clear_stage(StageAtomics &stage) noexcept;
	static void publish(StageAtomics &stage, int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns,
			    uint64_t timestamp_ns, uint64_t wall_ns, uint32_t unit_a, uint32_t unit_b,
			    uint32_t unit_c) noexcept;
	static StageSnapshot read_stage(const StageAtomics &stage) noexcept;
	static void write_csv_header(std::ostream &out);
	static void write_csv_row(std::ostream &out, const Sample &row);
	SchedulerSnapshot read_scheduler() const noexcept;

	std::atomic_bool enabled_{false};
	std::atomic<uint64_t> session_{0};
	std::atomic<uint8_t> last_event_{static_cast<uint8_t>(Event::None)};
	std::atomic<uint64_t> last_event_wall_ns_{0};
	std::atomic<uint64_t> event_generation_{0};
	StageAtomics capture_audio_;
	StageAtomics capture_video_;
	StageAtomics output_audio_;
	StageAtomics output_video_;
	StageAtomics filtered_audio_;
	StageAtomics selected_video_;
	SchedulerAtomics scheduler_;

	mutable std::mutex ring_mutex_;
	std::vector<Sample> ring_;
	size_t write_index_ = 0;
	size_t count_ = 0;
	uint64_t overwritten_ = 0;
	uint64_t sampled_event_generation_ = 0;
	std::string live_output_path_;
	std::ofstream live_output_;
	uint32_t live_rows_since_flush_ = 0;
};

} // namespace distroav::clocklab
