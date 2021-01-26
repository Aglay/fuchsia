// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CLOCK_MANAGER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CLOCK_MANAGER_H_

#include "src/media/audio/audio_core/audio_clock.h"

namespace media::audio {

//
// AudioClockManager provides a mechanism for relating all clocks created under a single manager
// instance.
//
// In AudioCore, an AudioClockManager instance is provided per-Context and facilitates creation of
// AudioClocks across AudioCore. Overriding the AudioClockManager class takes advantage of the
// single point-of-entry for clock creation, enabling sweeping AudioClock modifications and/or
// stubbing for tests.
//

class AudioClockManager {
 public:
  std::unique_ptr<AudioClock> CreateClientAdjustable(zx::clock clock) {
    return std::make_unique<AudioClock>(AudioClock::ClientAdjustable(std::move(clock)));
  }
  std::unique_ptr<AudioClock> CreateClientFixed(zx::clock clock) {
    return std::make_unique<AudioClock>(AudioClock::ClientFixed(std::move(clock)));
  }
  std::unique_ptr<AudioClock> CreateDeviceAdjustable(zx::clock clock, uint32_t domain) {
    return std::make_unique<AudioClock>(AudioClock::DeviceAdjustable(std::move(clock), domain));
  }
  std::unique_ptr<AudioClock> CreateDeviceFixed(zx::clock clock, uint32_t domain) {
    return std::make_unique<AudioClock>(AudioClock::DeviceFixed(std::move(clock), domain));
  }
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CLOCK_MANAGER_H_
