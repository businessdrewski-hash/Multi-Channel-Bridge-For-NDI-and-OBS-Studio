#pragma once

#include <obs-module.h>

#define BRIDGE_LOG(level, format, ...) \
  blog(level, "[NDI Multichannel Bridge] " format, ##__VA_ARGS__)

constexpr const char *kOutputId = "ndi_multichannel_bridge_output";
constexpr const char *kVideoSourceId = "ndi_multichannel_bridge_video";
constexpr const char *kAudioSourceId = "ndi_multichannel_bridge_audio_pair";
