#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

struct StereoAudioBlock {
  uint64_t timestamp_ns = 0;
  uint32_t sample_rate = 0;
  size_t frames = 0;
  std::array<std::vector<float>, 2> channels;
};

struct FourChannelAudioBlock {
  uint64_t timestamp_ns = 0;
  uint32_t sample_rate = 0;
  size_t frames = 0;
  std::array<std::vector<float>, 4> channels;
};

// Pairs OBS raw_audio2 callbacks from two selected mixer tracks. Callbacks for
// the same 1024-sample render quantum normally have identical timestamps, but
// a small tolerance makes the merger resilient to rounding differences.
class AudioPairAssembler {
public:
  explicit AudioPairAssembler(uint64_t tolerance_ns = 1'000'000ULL)
      : tolerance_ns_(tolerance_ns) {}

  std::optional<FourChannelAudioBlock> push(size_t slot, StereoAudioBlock block)
  {
    if (slot > 1 || block.frames == 0)
      return std::nullopt;

    queues_[slot].push_back(std::move(block));
    trim();
    return try_match();
  }

  void clear()
  {
    queues_[0].clear();
    queues_[1].clear();
  }

private:
  static uint64_t abs_diff(uint64_t a, uint64_t b)
  {
    return a > b ? a - b : b - a;
  }

  std::optional<FourChannelAudioBlock> try_match()
  {
    if (queues_[0].empty() || queues_[1].empty())
      return std::nullopt;

    size_t best_a = 0;
    size_t best_b = 0;
    uint64_t best_diff = UINT64_MAX;

    for (size_t a = 0; a < queues_[0].size(); ++a) {
      for (size_t b = 0; b < queues_[1].size(); ++b) {
        const uint64_t diff = abs_diff(queues_[0][a].timestamp_ns,
                                       queues_[1][b].timestamp_ns);
        if (diff < best_diff) {
          best_diff = diff;
          best_a = a;
          best_b = b;
        }
      }
    }

    if (best_diff > tolerance_ns_)
      return std::nullopt;

    StereoAudioBlock a = std::move(queues_[0][best_a]);
    StereoAudioBlock b = std::move(queues_[1][best_b]);
    queues_[0].erase(queues_[0].begin() + static_cast<std::ptrdiff_t>(best_a));
    queues_[1].erase(queues_[1].begin() + static_cast<std::ptrdiff_t>(best_b));

    const size_t frames = std::min(a.frames, b.frames);
    FourChannelAudioBlock out;
    out.timestamp_ns = std::min(a.timestamp_ns, b.timestamp_ns);
    out.sample_rate = a.sample_rate ? a.sample_rate : b.sample_rate;
    out.frames = frames;

    for (size_t ch = 0; ch < 2; ++ch) {
      out.channels[ch].assign(a.channels[ch].begin(),
                              a.channels[ch].begin() + static_cast<std::ptrdiff_t>(frames));
      out.channels[ch + 2].assign(b.channels[ch].begin(),
                                  b.channels[ch].begin() + static_cast<std::ptrdiff_t>(frames));
    }
    return out;
  }

  void trim()
  {
    constexpr size_t kMaxQueuedBlocks = 12;
    for (auto &queue : queues_) {
      while (queue.size() > kMaxQueuedBlocks)
        queue.pop_front();
    }
  }

  uint64_t tolerance_ns_;
  std::array<std::deque<StereoAudioBlock>, 2> queues_;
};
