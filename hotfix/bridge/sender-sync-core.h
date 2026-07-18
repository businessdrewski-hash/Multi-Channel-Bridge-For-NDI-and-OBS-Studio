#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mcb {

// OBS normally supplies 1024 frames per raw-output callback. Keeping a fixed
// 4096-frame ceiling covers unusually large blocks without permitting runtime
// allocation or unbounded memory growth on the real-time audio path.
class SenderSyncCore {
public:
	static constexpr size_t kChannelsPerMix = 2;
	static constexpr size_t kOutputChannels = 4;
	static constexpr size_t kQueueCapacity = 4;
	static constexpr size_t kMaxFrames = 4096;

	struct Output {
		uint64_t timestamp_ns = 0;
		uint32_t frames = 0;
		std::array<const float *, kOutputChannels> data{};
		bool silence_fallback = false;
	};

	struct Snapshot {
		uint64_t paired_blocks = 0;
		uint64_t discarded_blocks = 0;
		uint64_t silence_fallback_blocks = 0;
		uint64_t discontinuities = 0;
		uint64_t reanchors = 0;
		uint64_t oversized_blocks = 0;
		uint64_t epoch = 1;
		int64_t last_timestamp_delta_ns = 0;
		uint32_t queue_depth_a = 0;
		uint32_t queue_depth_b = 0;
		float peak_a = 0.0f;
		float peak_b = 0.0f;
	};

	void configure(uint32_t sample_rate) noexcept;
	void reset(bool reset_counters) noexcept;
	void reanchor() noexcept;

	// Copies one stereo OBS mix block into bounded preallocated storage. This
	// method never allocates, blocks, throws, or retains caller-owned pointers.
	bool push(size_t slot, uint64_t timestamp_ns, uint32_t frames,
		const float *left, const float *right, bool measure_peak) noexcept;

	// Returns at most one ready four-channel block. The returned pointers remain
	// valid until the next call to pop_output(), push(), reset(), or reanchor().
	bool pop_output(Output &output) noexcept;

	Snapshot snapshot() const noexcept;

private:
	struct StereoBlock {
		uint64_t timestamp_ns = 0;
		uint32_t frames = 0;
		float peak = 0.0f;
		std::array<float, kMaxFrames * kChannelsPerMix> samples{};
	};

	struct Queue {
		std::array<StereoBlock, kQueueCapacity> blocks{};
		size_t read = 0;
		size_t count = 0;
	};

	static uint64_t magnitude(int64_t value) noexcept;
	static float copy_channel(float *destination, const float *source,
		uint32_t frames, bool measure_peak) noexcept;
	static StereoBlock &front(Queue &queue) noexcept;
	static const StereoBlock &front(const Queue &queue) noexcept;
	static void pop(Queue &queue) noexcept;
	void clear_queues() noexcept;
	uint64_t block_duration_ns(uint32_t frames) const noexcept;
	bool timestamp_discontinuity(size_t slot, uint64_t timestamp_ns,
		uint32_t frames) const noexcept;
	bool pack_pair(const StereoBlock *a, const StereoBlock *b,
		Output &output, bool fallback) noexcept;

	uint32_t sample_rate_ = 48000;
	std::array<Queue, 2> queues_{};
	std::array<uint64_t, 2> last_timestamp_ns_{};
	std::array<float, kMaxFrames * kOutputChannels> packed_{};
	std::array<float, kMaxFrames> silence_{};
	Snapshot state_{};
};

} // namespace mcb
