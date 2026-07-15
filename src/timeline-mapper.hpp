#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>

// Maps an arbitrary NDI 100-nanosecond source timeline onto the local OBS
// monotonic nanosecond timeline. One instance is shared by video and all audio
// pairs from a receiver, so every output source gets the same clock mapping.
class TimelineMapper {
public:
  explicit TimelineMapper(uint64_t initial_latency_ns = 60'000'000ULL)
      : latency_ns_(initial_latency_ns) {}

  uint64_t map(int64_t ndi_ticks_100ns, uint64_t local_now_ns)
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!valid_ticks(ndi_ticks_100ns))
      return local_now_ns + latency_ns_;

    if (!anchored_) {
      anchor_ticks_ = ndi_ticks_100ns;
      anchor_local_ns_ = local_now_ns + latency_ns_;
      last_ticks_ = ndi_ticks_100ns;
      anchored_ = true;
      return anchor_local_ns_;
    }

    const int64_t delta_ticks = ndi_ticks_100ns - anchor_ticks_;
    const int64_t jump_ticks = ndi_ticks_100ns - last_ticks_;
    last_ticks_ = ndi_ticks_100ns;

    // Re-anchor on a backwards jump or a discontinuity larger than five seconds.
    if (jump_ticks < -10'000 || jump_ticks > 50'000'000) {
      anchor_ticks_ = ndi_ticks_100ns;
      anchor_local_ns_ = local_now_ns + latency_ns_;
      return anchor_local_ns_;
    }

    const int64_t mapped = static_cast<int64_t>(anchor_local_ns_) + delta_ticks * 100;
    return mapped > 0 ? static_cast<uint64_t>(mapped) : local_now_ns;
  }

  void reset()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    anchored_ = false;
    anchor_ticks_ = 0;
    last_ticks_ = 0;
    anchor_local_ns_ = 0;
  }

private:
  static bool valid_ticks(int64_t ticks)
  {
    // NDI undefined/synthesize sentinels are extreme values. Captured frames
    // should contain an ordinary positive timecode or timestamp.
    return ticks > 0 && ticks < (INT64_MAX / 4);
  }

  std::mutex mutex_;
  uint64_t latency_ns_ = 60'000'000ULL;
  bool anchored_ = false;
  int64_t anchor_ticks_ = 0;
  int64_t last_ticks_ = 0;
  uint64_t anchor_local_ns_ = 0;
};
