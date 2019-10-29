// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_LOADER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_LOADER_H_

#include <optional>

#include "src/media/audio/audio_core/process_config.h"

namespace media::audio {

class ProcessConfigLoader {
 public:
  // Loads a ProcessConfig from disk, defined according to audio_core_config_schema.jsx. The config
  // is expected to be correct and defined at build time. This will panic if the config file is
  // invalid.
  //
  // Returns the ProcessConfig if the file was present, or std::nullopt if the file was not present.
  static std::optional<ProcessConfig> LoadProcessConfig(const char* filename);
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_LOADER_H_
