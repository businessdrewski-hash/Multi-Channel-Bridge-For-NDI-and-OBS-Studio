#include "../hotfix/bridge/sender-sync-core.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

using mcb::SenderSyncCore;

static_assert(sizeof(SenderSyncCore) < 1024 * 1024,
	"Sender Sync Core must remain under the one-megabyte runtime-state budget");

namespace {
constexpr uint32_t frames = 1024;
constexpr uint64_t block_ns = 21333333ULL;

std::array<float, frames> left{};
std::array<float, frames> right{};

void push_pair(SenderSyncCore &core, uint64_t base)
{
	assert(core.push(0, base, frames, left.data(), right.data(), false));
	assert(core.push(1, base + block_ns, frames, left.data(), right.data(), false));
}
} // namespace

int main()
{
	SenderSyncCore core;
	core.configure(48000);

	left[0] = 0.5f;
	right[0] = -0.25f;
	assert(core.push(0, 1000000000ULL, frames, left.data(), right.data(), true));
	assert(core.push(1, 1000000000ULL + block_ns, frames, left.data(), right.data(), true));
	SenderSyncCore::Output output;
	assert(core.pop_output(output));
	assert(output.frames == frames);
	assert(output.timestamp_ns == 1000000000ULL);
	assert(!output.silence_fallback);
	assert(output.data[0][0] == 0.5f);
	assert(output.data[1][0] == -0.25f);
	assert(output.data[2][0] == 0.5f);
	auto state = core.snapshot();
	assert(state.paired_blocks == 1);
	assert(state.peak_a == 0.5f && state.peak_b == 0.5f);

	// Long-running normal pairing stays bounded and allocation-free by design.
	uint64_t timestamp = 1000000000ULL + block_ns * 2;
	for (int i = 0; i < 10000; ++i) {
		push_pair(core, timestamp);
		assert(core.pop_output(output));
		timestamp += block_ns * 2;
	}
	state = core.snapshot();
	assert(state.paired_blocks == 10001);
	assert(state.queue_depth_a == 0 && state.queue_depth_b == 0);

	// A backward timestamp creates a fresh epoch and clears stale queued data.
	assert(core.push(0, timestamp, frames, left.data(), right.data(), false));
	assert(core.push(1, timestamp + block_ns, frames, left.data(), right.data(), false));
	assert(core.pop_output(output));
	const uint64_t old_epoch = core.snapshot().epoch;
	assert(core.push(0, 500000000ULL, frames, left.data(), right.data(), false));
	state = core.snapshot();
	assert(state.discontinuities == 1);
	assert(state.epoch == old_epoch + 1);

	// Manual re-anchor is bounded and preserves counters.
	const uint64_t paired_before = state.paired_blocks;
	core.reanchor();
	state = core.snapshot();
	assert(state.reanchors == 1);
	assert(state.paired_blocks == paired_before);
	assert(state.queue_depth_a == 0 && state.queue_depth_b == 0);

	// A missing second mix falls back after three blocks without growing memory.
	const uint64_t fallback_start = 4000000000ULL;
	for (int i = 0; i < 3; ++i)
		assert(core.push(0, fallback_start + static_cast<uint64_t>(i) * block_ns * 2,
			frames, left.data(), right.data(), false));
	assert(core.pop_output(output));
	assert(output.silence_fallback);
	assert(output.data[2][0] == 0.0f && output.data[3][0] == 0.0f);

	// Oversized input is rejected before touching fixed storage.
	assert(!core.push(0, 9000000000ULL, SenderSyncCore::kMaxFrames + 1,
		left.data(), right.data(), false));
	assert(core.snapshot().oversized_blocks == 1);

	std::cout << "Sender Sync Core tests passed; state bytes=" << sizeof(SenderSyncCore) << "\n";
	return 0;
}
