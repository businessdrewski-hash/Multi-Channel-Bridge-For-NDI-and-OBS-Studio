#include "sender-sync-core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mcb {

uint64_t SenderSyncCore::magnitude(int64_t value) noexcept
{
	if (value >= 0)
		return static_cast<uint64_t>(value);
	return static_cast<uint64_t>(-(value + 1)) + 1;
}

float SenderSyncCore::copy_channel(float *destination, const float *source,
	uint32_t frames, bool measure_peak) noexcept
{
	if (!destination || frames == 0)
		return 0.0f;
	if (!source) {
		std::fill_n(destination, frames, 0.0f);
		return 0.0f;
	}
	std::memcpy(destination, source, static_cast<size_t>(frames) * sizeof(float));
	if (!measure_peak)
		return 0.0f;
	float peak = 0.0f;
	for (uint32_t i = 0; i < frames; ++i)
		peak = std::max(peak, std::fabs(source[i]));
	return std::min(peak, 1.0f);
}

SenderSyncCore::StereoBlock &SenderSyncCore::front(Queue &queue) noexcept
{
	return queue.blocks[queue.read];
}

const SenderSyncCore::StereoBlock &SenderSyncCore::front(const Queue &queue) noexcept
{
	return queue.blocks[queue.read];
}

void SenderSyncCore::pop(Queue &queue) noexcept
{
	if (!queue.count)
		return;
	queue.read = (queue.read + 1) % kQueueCapacity;
	--queue.count;
}

void SenderSyncCore::clear_queues() noexcept
{
	for (auto &queue : queues_) {
		queue.read = 0;
		queue.count = 0;
	}
	state_.queue_depth_a = 0;
	state_.queue_depth_b = 0;
}

uint64_t SenderSyncCore::block_duration_ns(uint32_t frames) const noexcept
{
	return sample_rate_ ? (static_cast<uint64_t>(frames) * 1000000000ULL) / sample_rate_ : 0;
}

bool SenderSyncCore::timestamp_discontinuity(size_t slot, uint64_t timestamp_ns,
	uint32_t frames) const noexcept
{
	const uint64_t previous = last_timestamp_ns_[slot];
	if (!previous)
		return false;
	if (!timestamp_ns || timestamp_ns <= previous)
		return true;
	const uint64_t duration = std::max<uint64_t>(block_duration_ns(frames), 1000000ULL);
	// Each selected OBS mixer normally advances by two raw-output blocks because
	// the shared counter advances once for each selected mixer callback. Sixteen
	// blocks leaves ample scheduling tolerance while still recognizing a restart.
	return timestamp_ns - previous > duration * 16ULL;
}

void SenderSyncCore::configure(uint32_t sample_rate) noexcept
{
	sample_rate_ = sample_rate >= 8000 && sample_rate <= 384000 ? sample_rate : 48000;
	reset(true);
}

void SenderSyncCore::reset(bool reset_counters) noexcept
{
	clear_queues();
	last_timestamp_ns_.fill(0);
	if (reset_counters) {
		state_ = {};
		state_.epoch = 1;
	} else {
		++state_.epoch;
		state_.last_timestamp_delta_ns = 0;
		state_.peak_a = 0.0f;
		state_.peak_b = 0.0f;
	}
}

void SenderSyncCore::reanchor() noexcept
{
	clear_queues();
	last_timestamp_ns_.fill(0);
	++state_.reanchors;
	++state_.epoch;
	state_.last_timestamp_delta_ns = 0;
}

bool SenderSyncCore::push(size_t slot, uint64_t timestamp_ns, uint32_t frames,
	const float *left, const float *right, bool measure_peak) noexcept
{
	if (slot > 1 || !frames || frames > kMaxFrames) {
		if (frames > kMaxFrames)
			++state_.oversized_blocks;
		return false;
	}
	if (timestamp_discontinuity(slot, timestamp_ns, frames)) {
		clear_queues();
		last_timestamp_ns_.fill(0);
		++state_.discontinuities;
		++state_.epoch;
	}
	last_timestamp_ns_[slot] = timestamp_ns;

	Queue &queue = queues_[slot];
	if (queue.count == kQueueCapacity) {
		pop(queue);
		++state_.discarded_blocks;
	}
	const size_t write = (queue.read + queue.count) % kQueueCapacity;
	StereoBlock &block = queue.blocks[write];
	block.timestamp_ns = timestamp_ns;
	block.frames = frames;
	const float left_peak = copy_channel(block.samples.data(), left, frames, measure_peak);
	const float right_peak = copy_channel(block.samples.data() + frames, right ? right : left,
		frames, measure_peak);
	block.peak = std::max(left_peak, right_peak);
	++queue.count;
	if (slot == 0)
		state_.peak_a = block.peak;
	else
		state_.peak_b = block.peak;
	state_.queue_depth_a = static_cast<uint32_t>(queues_[0].count);
	state_.queue_depth_b = static_cast<uint32_t>(queues_[1].count);
	return true;
}

bool SenderSyncCore::pack_pair(const StereoBlock *a, const StereoBlock *b,
	Output &output, bool fallback) noexcept
{
	const StereoBlock *reference = a ? a : b;
	if (!reference || !reference->frames)
		return false;
	const uint32_t frames = reference->frames;
	if ((a && a->frames != frames) || (b && b->frames != frames))
		return false;

	for (size_t channel = 0; channel < kChannelsPerMix; ++channel) {
		float *destination_a = packed_.data() + channel * frames;
		float *destination_b = packed_.data() + (channel + 2) * frames;
		const float *source_a = a ? a->samples.data() + channel * frames : silence_.data();
		const float *source_b = b ? b->samples.data() + channel * frames : silence_.data();
		std::memcpy(destination_a, source_a, static_cast<size_t>(frames) * sizeof(float));
		std::memcpy(destination_b, source_b, static_cast<size_t>(frames) * sizeof(float));
	}
	output.frames = frames;
	// OBS gives adjacent selected-mixer callbacks timestamps one block apart even
	// though their samples represent the same mix interval. The earlier timestamp
	// is the canonical interval start; using the later one adds a false block of
	// sender-side latency.
	output.timestamp_ns = a && b ? std::min(a->timestamp_ns, b->timestamp_ns)
		: reference->timestamp_ns;
	for (size_t channel = 0; channel < kOutputChannels; ++channel)
		output.data[channel] = packed_.data() + channel * frames;
	output.silence_fallback = fallback;
	return true;
}

bool SenderSyncCore::pop_output(Output &output) noexcept
{
	output = {};
	Queue &a_queue = queues_[0];
	Queue &b_queue = queues_[1];
	const uint64_t one_sample_ns = sample_rate_ ? 1000000000ULL / sample_rate_ : 20834ULL;

	while (a_queue.count && b_queue.count) {
		StereoBlock &a = front(a_queue);
		StereoBlock &b = front(b_queue);
		const int64_t delta = b.timestamp_ns >= a.timestamp_ns
			? static_cast<int64_t>(b.timestamp_ns - a.timestamp_ns)
			: -static_cast<int64_t>(a.timestamp_ns - b.timestamp_ns);
		const uint64_t duration = block_duration_ns(std::max(a.frames, b.frames));
		const uint64_t tolerance = duration + std::max<uint64_t>(500000ULL, one_sample_ns * 8ULL);
		state_.last_timestamp_delta_ns = delta;

		if (a.frames == b.frames && magnitude(delta) <= tolerance) {
			const bool ready = pack_pair(&a, &b, output, false);
			pop(a_queue);
			pop(b_queue);
			if (ready)
				++state_.paired_blocks;
			state_.queue_depth_a = static_cast<uint32_t>(a_queue.count);
			state_.queue_depth_b = static_cast<uint32_t>(b_queue.count);
			return ready;
		}

		if (a.timestamp_ns <= b.timestamp_ns)
			pop(a_queue);
		else
			pop(b_queue);
		++state_.discarded_blocks;
	}

	// Three queued blocks provide catch-up time without the former twelve-block
	// heap-backed queue. If one mix really stopped, transmit the healthy mix with
	// pre-zeroed channels so the NDI sender remains alive.
	constexpr size_t fallback_depth = 3;
	if (a_queue.count >= fallback_depth && !b_queue.count) {
		const bool ready = pack_pair(&front(a_queue), nullptr, output, true);
		pop(a_queue);
		if (ready)
			++state_.silence_fallback_blocks;
		state_.queue_depth_a = static_cast<uint32_t>(a_queue.count);
		return ready;
	}
	if (b_queue.count >= fallback_depth && !a_queue.count) {
		const bool ready = pack_pair(nullptr, &front(b_queue), output, true);
		pop(b_queue);
		if (ready)
			++state_.silence_fallback_blocks;
		state_.queue_depth_b = static_cast<uint32_t>(b_queue.count);
		return ready;
	}

	state_.queue_depth_a = static_cast<uint32_t>(a_queue.count);
	state_.queue_depth_b = static_cast<uint32_t>(b_queue.count);
	return false;
}

SenderSyncCore::Snapshot SenderSyncCore::snapshot() const noexcept
{
	return state_;
}

} // namespace mcb
