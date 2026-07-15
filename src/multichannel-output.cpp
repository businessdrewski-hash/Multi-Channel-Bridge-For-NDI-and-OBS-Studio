// Copyright (C) 2026 NDI Multichannel Bridge contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "multichannel-output.hpp"
#include "audio-pair-assembler.hpp"
#include "bridge-common.hpp"
#include "ndi-runtime.hpp"

#include <Processing.NDI.Lib.h>
#include <media-io/audio-io.h>
#include <media-io/video-io.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct MultichannelOutput {
  obs_output_t *output = nullptr;
  std::string ndi_name;
  std::string ndi_groups;
  size_t track_a = 0;
  size_t track_b = 1;
  std::atomic<bool> started{false};
  NDIlib_send_instance_t sender = nullptr;
  std::mutex sender_mutex;
  std::mutex audio_mutex;
  AudioPairAssembler assembler;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_num = 60;
  uint32_t fps_den = 1;
  uint32_t sample_rate = 48'000;
  std::array<std::vector<uint8_t>, 2> video_buffers;
  size_t next_video_buffer = 0;
  std::vector<float> audio_buffer;
  std::atomic<uint64_t> total_bytes{0};
};

const char *get_name(void *)
{
  return obs_module_text("Output.Name");
}

void defaults(obs_data_t *settings)
{
  obs_data_set_default_string(settings, "ndi_name", "OBS Multichannel A/V");
  obs_data_set_default_string(settings, "ndi_groups", "");
  obs_data_set_default_int(settings, "track_a", 0);
  obs_data_set_default_int(settings, "track_b", 1);
}

void update(void *data, obs_data_t *settings)
{
  auto *out = static_cast<MultichannelOutput *>(data);
  out->ndi_name = obs_data_get_string(settings, "ndi_name");
  out->ndi_groups = obs_data_get_string(settings, "ndi_groups");
  out->track_a = static_cast<size_t>(obs_data_get_int(settings, "track_a"));
  out->track_b = static_cast<size_t>(obs_data_get_int(settings, "track_b"));
}

void *create(obs_data_t *settings, obs_output_t *output)
{
  auto *out = new MultichannelOutput;
  out->output = output;
  update(out, settings);
  return out;
}

void stop(void *data, uint64_t)
{
  auto *out = static_cast<MultichannelOutput *>(data);
  if (!out->started.exchange(false))
    return;

  obs_output_end_data_capture(out->output);

  std::lock_guard<std::mutex> sender_lock(out->sender_mutex);
  if (out->sender) {
    // Flush the asynchronous video queue before destroying the sender.
    ndi_runtime::api()->send_send_video_async_v2(out->sender, nullptr);
    ndi_runtime::api()->send_destroy(out->sender);
    out->sender = nullptr;
  }

  {
    std::lock_guard<std::mutex> audio_lock(out->audio_mutex);
    out->assembler.clear();
  }
  BRIDGE_LOG(LOG_INFO, "Multichannel sender stopped");
}

void destroy(void *data)
{
  auto *out = static_cast<MultichannelOutput *>(data);
  if (!out)
    return;
  stop(out, 0);
  delete out;
}

bool start(void *data)
{
  auto *out = static_cast<MultichannelOutput *>(data);
  const auto *ndi = ndi_runtime::api();
  if (!ndi) {
    obs_output_set_last_error(out->output, "NDI runtime is not loaded");
    return false;
  }
  if (out->track_a == out->track_b || out->track_a >= MAX_AUDIO_MIXES ||
      out->track_b >= MAX_AUDIO_MIXES) {
    obs_output_set_last_error(out->output, "Choose two different OBS audio tracks");
    return false;
  }

  video_t *video = obs_output_video(out->output);
  audio_t *audio = obs_output_audio(out->output);
  if (!video || !audio) {
    obs_output_set_last_error(out->output, "OBS video or audio output is unavailable");
    return false;
  }

  out->width = video_output_get_width(video);
  out->height = video_output_get_height(video);
  const video_output_info *video_info = video_output_get_info(video);
  if (video_info) {
    out->fps_num = video_info->fps_num;
    out->fps_den = video_info->fps_den;
  }
  out->sample_rate = audio_output_get_sample_rate(audio);

  video_scale_info video_conversion{};
  video_conversion.format = VIDEO_FORMAT_BGRA;
  video_conversion.width = out->width;
  video_conversion.height = out->height;
  video_conversion.range = VIDEO_RANGE_FULL;
  video_conversion.colorspace = VIDEO_CS_SRGB;
  obs_output_set_video_conversion(out->output, &video_conversion);

  audio_convert_info audio_conversion{};
  audio_conversion.samples_per_sec = out->sample_rate;
  audio_conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
  audio_conversion.speakers = SPEAKERS_STEREO;
  obs_output_set_audio_conversion(out->output, &audio_conversion);

  NDIlib_send_create_t description{};
  description.p_ndi_name = out->ndi_name.c_str();
  description.p_groups = out->ndi_groups.empty() ? nullptr : out->ndi_groups.c_str();
  description.clock_video = false;
  description.clock_audio = false;

  {
    std::lock_guard<std::mutex> lock(out->sender_mutex);
    out->sender = ndi->send_create(&description);
  }
  if (!out->sender) {
    obs_output_set_last_error(out->output, "NDI sender creation failed");
    return false;
  }

  if (!obs_output_can_begin_data_capture(out->output, 0) ||
      !obs_output_begin_data_capture(out->output, OBS_OUTPUT_AV)) {
    obs_output_set_last_error(out->output, "OBS could not begin raw A/V capture");
    std::lock_guard<std::mutex> lock(out->sender_mutex);
    ndi->send_destroy(out->sender);
    out->sender = nullptr;
    return false;
  }

  out->started = true;
  out->total_bytes = 0;
  BRIDGE_LOG(LOG_INFO,
             "Started 4-channel NDI sender '%s': OBS track %zu -> NDI 1-2, track %zu -> NDI 3-4",
             out->ndi_name.c_str(), out->track_a + 1, out->track_b + 1);
  return true;
}

void raw_video(void *data, video_data *frame)
{
  auto *out = static_cast<MultichannelOutput *>(data);
  if (!out->started || !frame || !frame->data[0])
    return;

  const size_t stride = static_cast<size_t>(out->width) * 4;
  const size_t bytes = stride * out->height;
  auto &buffer = out->video_buffers[out->next_video_buffer];
  out->next_video_buffer ^= 1;
  buffer.resize(bytes);

  for (uint32_t y = 0; y < out->height; ++y) {
    std::memcpy(buffer.data() + static_cast<size_t>(y) * stride,
                frame->data[0] + static_cast<size_t>(y) * frame->linesize[0], stride);
  }

  NDIlib_video_frame_v2_t ndi_frame{};
  ndi_frame.xres = static_cast<int>(out->width);
  ndi_frame.yres = static_cast<int>(out->height);
  ndi_frame.FourCC = NDIlib_FourCC_video_type_BGRA;
  ndi_frame.frame_rate_N = static_cast<int>(out->fps_num);
  ndi_frame.frame_rate_D = static_cast<int>(out->fps_den);
  ndi_frame.picture_aspect_ratio = static_cast<float>(out->width) / static_cast<float>(out->height);
  ndi_frame.frame_format_type = NDIlib_frame_format_type_progressive;
  ndi_frame.timecode = NDIlib_send_timecode_synthesize;
  ndi_frame.p_data = buffer.data();
  ndi_frame.line_stride_in_bytes = static_cast<int>(stride);

  std::lock_guard<std::mutex> lock(out->sender_mutex);
  if (out->sender) {
    ndi_runtime::api()->send_send_video_async_v2(out->sender, &ndi_frame);
    out->total_bytes += bytes;
  }
}

StereoAudioBlock copy_stereo_block(audio_data *frame, uint32_t sample_rate)
{
  StereoAudioBlock block;
  block.timestamp_ns = frame->timestamp;
  block.sample_rate = sample_rate;
  block.frames = frame->frames;
  for (size_t channel = 0; channel < 2; ++channel) {
    block.channels[channel].resize(frame->frames);
    if (frame->data[channel]) {
      std::memcpy(block.channels[channel].data(), frame->data[channel],
                  frame->frames * sizeof(float));
    } else {
      std::fill(block.channels[channel].begin(), block.channels[channel].end(), 0.0f);
    }
  }
  return block;
}

void raw_audio2(void *data, size_t index, audio_data *frame)
{
  auto *out = static_cast<MultichannelOutput *>(data);
  if (!out->started || !frame)
    return;

  size_t slot = 2;
  if (index == out->track_a)
    slot = 0;
  else if (index == out->track_b)
    slot = 1;
  else
    return;

  std::optional<FourChannelAudioBlock> merged;
  {
    std::lock_guard<std::mutex> lock(out->audio_mutex);
    merged = out->assembler.push(slot, copy_stereo_block(frame, out->sample_rate));
  }
  if (!merged)
    return;

  const size_t frames = merged->frames;
  out->audio_buffer.resize(frames * 4);
  for (size_t channel = 0; channel < 4; ++channel) {
    std::memcpy(out->audio_buffer.data() + channel * frames,
                merged->channels[channel].data(), frames * sizeof(float));
  }

  NDIlib_audio_frame_v3_t ndi_frame{};
  ndi_frame.sample_rate = static_cast<int>(merged->sample_rate);
  ndi_frame.no_channels = 4;
  ndi_frame.no_samples = static_cast<int>(frames);
  ndi_frame.timecode = NDIlib_send_timecode_synthesize;
  ndi_frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
  ndi_frame.p_data = reinterpret_cast<uint8_t *>(out->audio_buffer.data());
  ndi_frame.channel_stride_in_bytes = static_cast<int>(frames * sizeof(float));

  std::lock_guard<std::mutex> lock(out->sender_mutex);
  if (out->sender) {
    ndi_runtime::api()->send_send_audio_v3(out->sender, &ndi_frame);
    out->total_bytes += frames * 4 * sizeof(float);
  }
}

uint64_t total_bytes(void *data)
{
  return static_cast<MultichannelOutput *>(data)->total_bytes.load();
}

} // namespace

obs_output_info create_multichannel_output_info()
{
  obs_output_info info{};
  info.id = kOutputId;
  info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_MULTI_TRACK;
  info.get_name = get_name;
  info.create = create;
  info.destroy = destroy;
  info.start = start;
  info.stop = stop;
  info.raw_video = raw_video;
  info.raw_audio2 = raw_audio2;
  info.update = update;
  info.get_defaults = defaults;
  info.get_total_bytes = total_bytes;
  return info;
}
