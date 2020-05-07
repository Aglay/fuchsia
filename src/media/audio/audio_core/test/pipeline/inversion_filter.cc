// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

// This is a simple inversion effect. The configuration string gives the maximum sample value.
// Every sample value larger than the max is clipped to the max.

#include <lib/media/audio/effects/audio_effects.h>
#include <lib/syslog/cpp/macros.h>

#include <cstring>
#include <string>

#include <rapidjson/document.h>

// A simple implementation of <lib/media/audio/effects/audio_effects.h>.
namespace {

struct Inverter {
  uint32_t frame_rate;
  uint16_t channels;
};

bool inverter_get_info(uint32_t effect_id, fuchsia_audio_effects_description* desc) {
  if (effect_id != 0 || desc == nullptr) {
    return false;
  }
  strlcpy(desc->name, "inversion_filter", sizeof(desc->name));
  desc->incoming_channels = FUCHSIA_AUDIO_EFFECTS_CHANNELS_ANY;
  desc->outgoing_channels = FUCHSIA_AUDIO_EFFECTS_CHANNELS_SAME_AS_IN;
  return true;
}

fuchsia_audio_effects_handle_t inverter_create(uint32_t effect_id, uint32_t frame_rate,
                                               uint16_t channels_in, uint16_t channels_out,
                                               const char* config, size_t config_length) {
  if (effect_id != 0 || channels_in != channels_out) {
    return FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE;
  }
  auto i = new Inverter{
      .frame_rate = frame_rate,
      .channels = channels_in,
  };
  return reinterpret_cast<fuchsia_audio_effects_handle_t>(i);
}

bool inverter_update_configuration(fuchsia_audio_effects_handle_t handle, const char* config,
                                   size_t config_length) {
  return handle != FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE;
}

bool inverter_delete(fuchsia_audio_effects_handle_t handle) {
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE) {
    return false;
  }
  delete reinterpret_cast<Inverter*>(handle);
  return true;
}

bool inverter_get_parameters(fuchsia_audio_effects_handle_t handle,
                             fuchsia_audio_effects_parameters* params) {
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE || params == nullptr) {
    return false;
  }

  auto i = reinterpret_cast<Inverter*>(handle);
  memset(params, 0, sizeof(*params));
  params->frame_rate = i->frame_rate;
  params->channels_in = i->channels;
  params->channels_out = i->channels;
  params->block_size_frames = FUCHSIA_AUDIO_EFFECTS_BLOCK_SIZE_ANY;
  params->signal_latency_frames = 0;
  params->max_frames_per_buffer = 0;
  return true;
}

bool inverter_process_inplace(fuchsia_audio_effects_handle_t handle, uint32_t num_frames,
                              float* audio_buff_in_out) {
  if (handle == FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE || audio_buff_in_out == nullptr) {
    return false;
  }

  auto i = reinterpret_cast<Inverter*>(handle);
  for (uint32_t k = 0; k < num_frames * i->channels; k++) {
    audio_buff_in_out[k] = -audio_buff_in_out[k];
  }
  return true;
}

bool inverter_process(fuchsia_audio_effects_handle_t handle, uint32_t num_frames,
                      const float* audio_buff_in, float** audio_buff_out) {
  return false;  // this library supports in-place effects only
}

bool inverter_flush(fuchsia_audio_effects_handle_t handle) {
  return handle != FUCHSIA_AUDIO_EFFECTS_INVALID_HANDLE;
}

}  // namespace

DECLARE_FUCHSIA_AUDIO_EFFECTS_MODULE_V1{
    1,  // num_effects
    &inverter_get_info,
    &inverter_create,
    &inverter_update_configuration,
    &inverter_delete,
    &inverter_get_parameters,
    &inverter_process_inplace,
    &inverter_process,
    &inverter_flush,
};
