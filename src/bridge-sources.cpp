// Copyright (C) 2026 NDI Multichannel Bridge contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bridge-sources.hpp"
#include "bridge-common.hpp"
#include "receiver-hub.hpp"

#include <algorithm>
#include <memory>
#include <string>

namespace {

struct BridgeSource {
  obs_source_t *source = nullptr;
  ReceiverHub::Role role = ReceiverHub::Role::Video;
  int pair_start = 0;
  std::string ndi_source_name;
  std::shared_ptr<ReceiverHub> hub;
};

const char *video_name(void *)
{
  return obs_module_text("Source.VideoName");
}

const char *audio_name(void *)
{
  return obs_module_text("Source.AudioName");
}

void reconnect(BridgeSource *bridge, obs_data_t *settings)
{
  const std::string requested_name = obs_data_get_string(settings, "ndi_source_name");
  const int requested_pair = static_cast<int>(obs_data_get_int(settings, "pair_start"));

  if (bridge->hub)
    bridge->hub->unsubscribe(bridge->source);
  bridge->hub.reset();

  bridge->ndi_source_name = requested_name;
  bridge->pair_start = requested_pair == 2 ? 2 : 0;
  if (!bridge->ndi_source_name.empty()) {
    bridge->hub = ReceiverHub::acquire(bridge->ndi_source_name);
    if (bridge->hub)
      bridge->hub->subscribe(bridge->source, bridge->role, bridge->pair_start);
  }
}

void *create_source(obs_data_t *settings, obs_source_t *source, ReceiverHub::Role role)
{
  auto *bridge = new BridgeSource;
  bridge->source = source;
  bridge->role = role;
  reconnect(bridge, settings);
  return bridge;
}

void *create_video(obs_data_t *settings, obs_source_t *source)
{
  return create_source(settings, source, ReceiverHub::Role::Video);
}

void *create_audio(obs_data_t *settings, obs_source_t *source)
{
  return create_source(settings, source, ReceiverHub::Role::AudioPair);
}

void destroy_source(void *data)
{
  auto *bridge = static_cast<BridgeSource *>(data);
  if (bridge->hub)
    bridge->hub->unsubscribe(bridge->source);
  delete bridge;
}

void update_source(void *data, obs_data_t *settings)
{
  reconnect(static_cast<BridgeSource *>(data), settings);
}

void defaults_video(obs_data_t *settings)
{
  obs_data_set_default_string(settings, "ndi_source_name", "");
}

void defaults_audio(obs_data_t *settings)
{
  obs_data_set_default_string(settings, "ndi_source_name", "");
  obs_data_set_default_int(settings, "pair_start", 0);
}

obs_properties_t *properties_video(void *)
{
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_text(props, "ndi_source_name", obs_module_text("Source.NDIName"), OBS_TEXT_DEFAULT);
  return props;
}

obs_properties_t *properties_audio(void *)
{
  obs_properties_t *props = properties_video(nullptr);
  obs_property_t *pair = obs_properties_add_list(props, "pair_start", obs_module_text("Source.Pair"),
                                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(pair, obs_module_text("Source.Pair12"), 0);
  obs_property_list_add_int(pair, obs_module_text("Source.Pair34"), 2);
  return props;
}

uint32_t width(void *data)
{
  auto *bridge = static_cast<BridgeSource *>(data);
  return bridge->hub ? bridge->hub->width() : 0;
}

uint32_t height(void *data)
{
  auto *bridge = static_cast<BridgeSource *>(data);
  return bridge->hub ? bridge->hub->height() : 0;
}

} // namespace

obs_source_info create_bridge_video_source_info()
{
  obs_source_info info{};
  info.id = kVideoSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
  info.get_name = video_name;
  info.create = create_video;
  info.destroy = destroy_source;
  info.update = update_source;
  info.get_defaults = defaults_video;
  info.get_properties = properties_video;
  info.get_width = width;
  info.get_height = height;
  return info;
}

obs_source_info create_bridge_audio_source_info()
{
  obs_source_info info{};
  info.id = kAudioSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_AUDIO;
  info.get_name = audio_name;
  info.create = create_audio;
  info.destroy = destroy_source;
  info.update = update_source;
  info.get_defaults = defaults_audio;
  info.get_properties = properties_audio;
  return info;
}
