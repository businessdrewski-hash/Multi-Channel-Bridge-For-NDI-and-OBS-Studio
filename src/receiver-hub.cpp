// Copyright (C) 2026 NDI Multichannel Bridge contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "receiver-hub.hpp"
#include "bridge-common.hpp"
#include "ndi-runtime.hpp"

#include <Processing.NDI.Lib.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace {
std::mutex g_hubs_mutex;
std::unordered_map<std::string, std::weak_ptr<ReceiverHub>> g_hubs;

video_format obs_format_from_ndi(NDIlib_FourCC_video_type_e fourcc)
{
  switch (fourcc) {
  case NDIlib_FourCC_video_type_BGRA:
    return VIDEO_FORMAT_BGRA;
  case NDIlib_FourCC_video_type_BGRX:
    return VIDEO_FORMAT_BGRX;
  case NDIlib_FourCC_video_type_RGBA:
  case NDIlib_FourCC_video_type_RGBX:
    return VIDEO_FORMAT_RGBA;
  case NDIlib_FourCC_video_type_UYVY:
    return VIDEO_FORMAT_UYVY;
  case NDIlib_FourCC_video_type_I420:
    return VIDEO_FORMAT_I420;
  case NDIlib_FourCC_video_type_NV12:
    return VIDEO_FORMAT_NV12;
  default:
    return VIDEO_FORMAT_NONE;
  }
}
} // namespace

std::shared_ptr<ReceiverHub> ReceiverHub::acquire(const std::string &ndi_source_name)
{
  if (ndi_source_name.empty())
    return {};
  std::lock_guard<std::mutex> lock(g_hubs_mutex);
  auto &entry = g_hubs[ndi_source_name];
  if (auto existing = entry.lock())
    return existing;
  auto created = std::make_shared<ReceiverHub>(ndi_source_name);
  entry = created;
  return created;
}

ReceiverHub::ReceiverHub(std::string ndi_source_name)
    : ndi_source_name_(std::move(ndi_source_name)), thread_(&ReceiverHub::run, this)
{
}

ReceiverHub::~ReceiverHub()
{
  running_ = false;
  if (thread_.joinable())
    thread_.join();

  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  for (auto &subscriber : subscribers_) {
    if (subscriber.weak_source)
      obs_weak_source_release(subscriber.weak_source);
  }
  subscribers_.clear();
}

void ReceiverHub::subscribe(obs_source_t *source, Role role, int pair_start_channel)
{
  if (!source)
    return;
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  Subscriber subscriber;
  subscriber.weak_source = obs_source_get_weak_source(source);
  subscriber.role = role;
  subscriber.pair_start_channel = pair_start_channel;
  subscribers_.push_back(subscriber);
}

void ReceiverHub::unsubscribe(obs_source_t *source)
{
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  auto it = subscribers_.begin();
  while (it != subscribers_.end()) {
    if (obs_weak_source_references_source(it->weak_source, source)) {
      obs_weak_source_release(it->weak_source);
      it = subscribers_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<ReceiverHub::StrongTarget> ReceiverHub::targets_snapshot()
{
  std::vector<StrongTarget> targets;
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  for (const auto &subscriber : subscribers_) {
    obs_source_t *source = obs_weak_source_get_source(subscriber.weak_source);
    if (source)
      targets.push_back({source, subscriber.role, subscriber.pair_start_channel});
  }
  return targets;
}

int64_t ReceiverHub::frame_clock_ticks(int64_t timecode, int64_t timestamp) const
{
  if (timecode > 0 && timecode < INT64_MAX / 4)
    return timecode;
  return timestamp;
}

void ReceiverHub::output_video(const NDIlib_video_frame_v2_t &frame)
{
  const video_format format = obs_format_from_ndi(frame.FourCC);
  if (format == VIDEO_FORMAT_NONE)
    return;

  const uint64_t timestamp_ns = timeline_.map(
      frame_clock_ticks(frame.timecode, frame.timestamp), os_gettime_ns());
  width_ = static_cast<uint32_t>(frame.xres);
  height_ = static_cast<uint32_t>(frame.yres);

  obs_source_frame obs_frame{};
  obs_frame.format = format;
  obs_frame.width = static_cast<uint32_t>(frame.xres);
  obs_frame.height = static_cast<uint32_t>(frame.yres);
  obs_frame.timestamp = timestamp_ns;
  obs_frame.data[0] = frame.p_data;
  obs_frame.linesize[0] = static_cast<uint32_t>(frame.line_stride_in_bytes);

  auto targets = targets_snapshot();
  for (auto &target : targets) {
    if (target.role == Role::Video)
      obs_source_output_video(target.source, &obs_frame);
    obs_source_release(target.source);
  }
}

void ReceiverHub::output_audio(const NDIlib_audio_frame_v3_t &frame)
{
  if (frame.no_samples <= 0 || frame.sample_rate <= 0 || !frame.p_data)
    return;

  const uint64_t timestamp_ns = timeline_.map(
      frame_clock_ticks(frame.timecode, frame.timestamp), os_gettime_ns());
  const size_t samples = static_cast<size_t>(frame.no_samples);

  auto targets = targets_snapshot();
  for (auto &target : targets) {
    if (target.role != Role::AudioPair) {
      obs_source_release(target.source);
      continue;
    }

    std::array<std::vector<float>, 2> pair_buffers;
    for (int pair_channel = 0; pair_channel < 2; ++pair_channel) {
      const int source_channel = target.pair_start_channel + pair_channel;
      pair_buffers[pair_channel].resize(samples);
      if (source_channel >= 0 && source_channel < frame.no_channels) {
        const auto *source_data = reinterpret_cast<const float *>(
            frame.p_data + source_channel * frame.channel_stride_in_bytes);
        std::memcpy(pair_buffers[pair_channel].data(), source_data,
                    samples * sizeof(float));
      } else {
        std::fill(pair_buffers[pair_channel].begin(), pair_buffers[pair_channel].end(), 0.0f);
      }
    }

    obs_source_audio obs_audio{};
    obs_audio.data[0] = reinterpret_cast<uint8_t *>(pair_buffers[0].data());
    obs_audio.data[1] = reinterpret_cast<uint8_t *>(pair_buffers[1].data());
    obs_audio.frames = static_cast<uint32_t>(samples);
    obs_audio.samples_per_sec = static_cast<uint32_t>(frame.sample_rate);
    obs_audio.speakers = SPEAKERS_STEREO;
    obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    obs_audio.timestamp = timestamp_ns;
    obs_source_output_audio(target.source, &obs_audio);
    obs_source_release(target.source);
  }
}

void ReceiverHub::run()
{
  const auto *ndi = ndi_runtime::api();
  if (!ndi)
    return;

  NDIlib_recv_create_v3_t description{};
  description.source_to_connect_to.p_ndi_name = ndi_source_name_.c_str();
  description.source_to_connect_to.p_url_address = nullptr;
  description.color_format = NDIlib_recv_color_format_UYVY_BGRA;
  description.bandwidth = NDIlib_recv_bandwidth_highest;
  description.allow_video_fields = false;
  description.p_ndi_recv_name = "OBS NDI Multichannel Bridge";

  NDIlib_recv_instance_t receiver = ndi->recv_create_v3(&description);
  if (!receiver) {
    BRIDGE_LOG(LOG_ERROR, "Could not create NDI receiver for '%s'", ndi_source_name_.c_str());
    return;
  }

  BRIDGE_LOG(LOG_INFO, "Receiver hub connected to '%s'", ndi_source_name_.c_str());
  while (running_) {
    NDIlib_video_frame_v2_t video{};
    NDIlib_audio_frame_v3_t audio{};
    NDIlib_metadata_frame_t metadata{};

    const NDIlib_frame_type_e type =
        ndi->recv_capture_v3(receiver, &video, &audio, &metadata, 100);
    switch (type) {
    case NDIlib_frame_type_video:
      output_video(video);
      ndi->recv_free_video_v2(receiver, &video);
      break;
    case NDIlib_frame_type_audio:
      output_audio(audio);
      ndi->recv_free_audio_v3(receiver, &audio);
      break;
    case NDIlib_frame_type_metadata:
      ndi->recv_free_metadata(receiver, &metadata);
      break;
    case NDIlib_frame_type_error:
      BRIDGE_LOG(LOG_WARNING, "NDI receiver reported an error for '%s'; reconnecting",
                 ndi_source_name_.c_str());
      running_ = false;
      break;
    default:
      break;
    }
  }

  ndi->recv_destroy(receiver);
}
