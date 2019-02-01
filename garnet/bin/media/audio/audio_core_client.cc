// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/media/audio/audio_core_client.h"

namespace media::audio {
AudioCoreClient::AudioCoreClient(component::StartupContext* startup_context,
                                 fit::closure quit_callback)
    : quit_callback_(std::move(quit_callback)) {
  audio_core_.set_error_handler([this](zx_status_t status) {
    FXL_LOG(INFO) << "System error: AudioCore service failure: " << status;
    quit_callback_();
  });

  startup_context->ConnectToEnvironmentService(audio_core_.NewRequest());

  audio_core_.events().SystemGainMuteChanged = [this](float gain, bool muted) {
    system_gain_db_ = gain;
    system_muted_ = muted;
    NotifyGainMuteChanged();
  };

  startup_context->outgoing().AddPublicService<fuchsia::media::Audio>(
      [this](fidl::InterfaceRequest<fuchsia::media::Audio> request) {
        bindings_.AddBinding(
            this,
            fidl::InterfaceRequest<fuchsia::media::Audio>(std::move(request)));
        bindings_.bindings().back()->events().SystemGainMuteChanged(
            system_gain_db_, system_muted_);
      });
}

void AudioCoreClient::NotifyGainMuteChanged() {
  for (auto& binding : bindings_.bindings()) {
    binding->events().SystemGainMuteChanged(system_gain_db_, system_muted_);
  }
}

}  // namespace media::audio
