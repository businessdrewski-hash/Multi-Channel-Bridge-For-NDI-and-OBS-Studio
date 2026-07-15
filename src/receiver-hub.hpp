#pragma once

#include "timeline-mapper.hpp"

#include <Processing.NDI.Lib.h>
#include <obs.h>

#include <cstdint>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ReceiverHub : public std::enable_shared_from_this<ReceiverHub> {
public:
  enum class Role { Video, AudioPair };

  static std::shared_ptr<ReceiverHub> acquire(const std::string &ndi_source_name);

  explicit ReceiverHub(std::string ndi_source_name);
  ~ReceiverHub();

  ReceiverHub(const ReceiverHub &) = delete;
  ReceiverHub &operator=(const ReceiverHub &) = delete;

  void subscribe(obs_source_t *source, Role role, int pair_start_channel);
  void unsubscribe(obs_source_t *source);

  uint32_t width() const { return width_.load(); }
  uint32_t height() const { return height_.load(); }
  const std::string &name() const { return ndi_source_name_; }

private:
  struct Subscriber {
    obs_weak_source_t *weak_source = nullptr;
    Role role = Role::Video;
    int pair_start_channel = 0;
  };

  struct StrongTarget {
    obs_source_t *source = nullptr;
    Role role = Role::Video;
    int pair_start_channel = 0;
  };

  void run();
  std::vector<StrongTarget> targets_snapshot();
  void output_video(const NDIlib_video_frame_v2_t &frame);
  void output_audio(const NDIlib_audio_frame_v3_t &frame);
  int64_t frame_clock_ticks(int64_t timecode, int64_t timestamp) const;

  std::string ndi_source_name_;
  std::atomic<bool> running_{true};
  std::thread thread_;
  mutable std::mutex subscribers_mutex_;
  std::vector<Subscriber> subscribers_;
  TimelineMapper timeline_;
  std::atomic<uint32_t> width_{0};
  std::atomic<uint32_t> height_{0};
};
