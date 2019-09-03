// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CORE_IMPL_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CORE_IMPL_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fzl/vmar-manager.h>
#include <lib/sys/cpp/component_context.h>

#include <mutex>

#include <fbl/intrusive_double_list.h>
#include <fbl/unique_ptr.h>
#include <trace/event.h>

#include "lib/fidl/cpp/binding_set.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/audio_admin.h"
#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/audio_packet_ref.h"
#include "src/media/audio/audio_core/command_line_options.h"
#include "src/media/audio/audio_core/fwd_decls.h"
#include "src/media/audio/audio_core/pending_flush_token.h"

namespace component {
class Services;
}

namespace media::audio {

class SystemGainMuteProvider {
 public:
  virtual float system_gain_db() const = 0;
  virtual bool system_muted() const = 0;
};

class UsageGainAdjustment {
 public:
  virtual void SetRenderUsageGainAdjustment(fuchsia::media::AudioRenderUsage usage,
                                            float gain_db) = 0;
  virtual void SetCaptureUsageGainAdjustment(fuchsia::media::AudioCaptureUsage, float gain_db) = 0;
};

class AudioCoreImpl : public fuchsia::media::AudioCore,
                      SystemGainMuteProvider,
                      UsageGainAdjustment {
 public:
  AudioCoreImpl(std::unique_ptr<sys::ComponentContext> component_context,
                CommandLineOptions options);
  ~AudioCoreImpl() override;

  // Audio implementation.
  void CreateAudioRenderer(
      fidl::InterfaceRequest<fuchsia::media::AudioRenderer> audio_renderer_request) final;

  void CreateAudioCapturer(
      bool loopback,
      fidl::InterfaceRequest<fuchsia::media::AudioCapturer> audio_capturer_request) final;

  void SetSystemGain(float gain_db) final;
  void SetSystemMute(bool muted) final;

  void SetRoutingPolicy(fuchsia::media::AudioOutputRoutingPolicy policy) final;

  void EnableDeviceSettings(bool enabled) final;

  // Schedule a closure to run on the service's main message loop.
  void ScheduleMainThreadTask(fit::closure task) {
    FXL_DCHECK(dispatcher_);
    async::PostTask(dispatcher_, std::move(task));
  }

  async_dispatcher_t* dispatcher() const { return dispatcher_; }
  AudioDeviceManager& device_manager() { return device_manager_; }
  AudioAdmin& audio_admin() { return audio_admin_; }

  float system_gain_db() const override { return system_gain_db_; }
  bool system_muted() const override { return system_muted_; }

  fbl::RefPtr<fzl::VmarManager> vmar() const { return vmar_manager_; }

  // Usage related fidl calls
  void SetInteraction(fuchsia::media::Usage active, fuchsia::media::Usage affected,
                      fuchsia::media::Behavior behavior) final;
  void ResetInteractions() final;
  void LoadDefaults() final;

  void SetRenderUsageGain(fuchsia::media::AudioRenderUsage usage, float gain_db) final;
  void SetCaptureUsageGain(fuchsia::media::AudioCaptureUsage usage, float gain_db) final;

 private:
  // |UsageGainAdjustment|
  void SetRenderUsageGainAdjustment(fuchsia::media::AudioRenderUsage usage, float gain_db) override;
  void SetCaptureUsageGainAdjustment(fuchsia::media::AudioCaptureUsage usage,
                                     float gain_db) override;

  static constexpr float kDefaultSystemGainDb = -12.0f;
  static constexpr bool kDefaultSystemMuted = false;
  static constexpr float kMaxSystemAudioGainDb = Gain::kUnityGainDb;

  void NotifyGainMuteChanged();
  void PublishServices();
  void Shutdown();

  fidl::BindingSet<fuchsia::media::AudioCore> bindings_;

  // A reference to our thread's dispatcher object.  Allows us to post events to
  // be handled by our main application thread from things like the output
  // manager's thread pool.
  async_dispatcher_t* dispatcher_;

  // State for dealing with devices.
  AudioDeviceManager device_manager_;

  // Audio usage manager
  AudioAdmin audio_admin_;

  std::unique_ptr<sys::ComponentContext> component_context_;

  // TODO(johngro): remove this state.  Migrate users to AudioDeviceEnumerator,
  // to control gain on a per-input/output basis.
  // Either way, Gain and Mute should remain fully independent.
  float system_gain_db_ = kDefaultSystemGainDb;
  bool system_muted_ = kDefaultSystemMuted;

  // We allocate a sub-vmar to hold the audio renderer buffers. Keeping these
  // in a sub-vmar allows us to take advantage of ASLR while minimizing page
  // table fragmentation.
  fbl::RefPtr<fzl::VmarManager> vmar_manager_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AudioCoreImpl);
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CORE_IMPL_H_
