// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_core_impl.h"

#include <fuchsia/scheduler/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "src/media/audio/audio_core/audio_capturer_impl.h"
#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/audio_renderer_impl.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {
namespace {
// All audio renderer buffers will need to fit within this VMAR. We want to
// choose a size here large enough that will accomodate all the mappings
// required by all clients while also being small enough to avoid unnecessary
// page table fragmentation.
constexpr size_t kAudioRendererVmarSize = 16ull * 1024 * 1024 * 1024;
constexpr zx_vm_option_t kAudioRendererVmarFlags =
    ZX_VM_COMPACT | ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE | ZX_VM_ALIGN_1GB;
}  // namespace

constexpr float AudioCoreImpl::kMaxSystemAudioGainDb;

AudioCoreImpl::AudioCoreImpl(std::unique_ptr<sys::ComponentContext> component_context,
                             CommandLineOptions options)
    : device_manager_(this),
      audio_admin_(this),
      component_context_(std::move(component_context)),
      vmar_manager_(
          fzl::VmarManager::Create(kAudioRendererVmarSize, nullptr, kAudioRendererVmarFlags)) {
  FXL_DCHECK(vmar_manager_ != nullptr) << "Failed to allocate VMAR";

  AudioDeviceSettings::EnableDeviceSettings(options.enable_device_settings_writeback);

#ifdef NDEBUG
  Logging::Init(fxl::LOG_WARNING);
#else
  // For verbose logging, set to -media::audio::TRACE or -media::audio::SPEW
  Logging::Init(fxl::LOG_INFO);
#endif

  // Stash a pointer to our async object.
  dispatcher_ = async_get_default_dispatcher();
  FXL_DCHECK(dispatcher_);

  // TODO(30888)
  //
  // Eliminate this as soon as we have a more official way of meeting real-time latency
  // requirements.  The main async_t is responsible for receiving audio payloads sent by
  // applications, so it has real time requirements (just like the mixing threads do).  In a perfect
  // world, however, we would want to have this task run on a thread which is different from the
  // thread which is processing *all* audio service jobs (even non-realtime ones).  This, however,
  // will take more significant restructuring.  We will cross that bridge when we have the TBD way
  // to deal with realtime requirements in place.
  AcquireAudioCoreImplProfile(component_context_.get(), [](zx::profile profile) {
    FXL_DCHECK(profile);
    if (profile) {
      zx_status_t status = zx::thread::self()->set_profile(profile, 0);
      FXL_DCHECK(status == ZX_OK);
    }
  });

  // Set up our output manager.
  zx_status_t res = device_manager_.Init();
  // TODO(johngro): Do better at error handling than this weak check.
  FXL_DCHECK(res == ZX_OK);

  // Set up our audio policy.
  res = audio_admin_.Init();
  FXL_DCHECK(res == ZX_OK);

  PublishServices();
}

AudioCoreImpl::~AudioCoreImpl() {
  Shutdown();
  FXL_DCHECK(packet_cleanup_queue_.is_empty());
  FXL_DCHECK(flush_cleanup_queue_.is_empty());
}

void AudioCoreImpl::PublishServices() {
  component_context_->outgoing()->AddPublicService<fuchsia::media::AudioCore>(
      [this](fidl::InterfaceRequest<fuchsia::media::AudioCore> request) {
        bindings_.AddBinding(this, std::move(request));
        bindings_.bindings().back()->events().SystemGainMuteChanged(system_gain_db_, system_muted_);
      });
  // TODO(dalesat): Load the gain/mute values.

  component_context_->outgoing()->AddPublicService<fuchsia::media::AudioDeviceEnumerator>(
      [this](fidl::InterfaceRequest<fuchsia::media::AudioDeviceEnumerator> request) {
        device_manager_.AddDeviceEnumeratorClient(std::move(request));
      });
}

void AudioCoreImpl::Shutdown() {
  TRACE_DURATION("audio", "AudioCoreImpl::Shutdown");
  shutting_down_ = true;
  device_manager_.Shutdown();
  DoPacketCleanup(0);
}

void AudioCoreImpl::CreateAudioRenderer(
    fidl::InterfaceRequest<fuchsia::media::AudioRenderer> audio_renderer_request) {
  TRACE_DURATION("audio", "AudioCoreImpl::CreateAudioRenderer");
  AUD_VLOG(TRACE);
  device_manager_.AddAudioRenderer(
      AudioRendererImpl::Create(std::move(audio_renderer_request), this));
}

void AudioCoreImpl::CreateAudioCapturer(
    bool loopback, fidl::InterfaceRequest<fuchsia::media::AudioCapturer> audio_capturer_request) {
  TRACE_DURATION("audio", "AudioCoreImpl::CreateAudioCapturer");
  AUD_VLOG(TRACE);
  device_manager_.AddAudioCapturer(
      AudioCapturerImpl::Create(loopback, std::move(audio_capturer_request), this));
}

void AudioCoreImpl::SetSystemGain(float gain_db) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetSystemGain");
  // NAN is undefined and "signless". We cannot simply clamp it into range.
  AUD_VLOG(TRACE) << " (" << gain_db << " dB)";
  if (isnan(gain_db)) {
    FXL_LOG(ERROR) << "Invalid system gain " << gain_db << " dB -- making no change";
    return;
  }
  gain_db =
      std::max(std::min(gain_db, kMaxSystemAudioGainDb), fuchsia::media::audio::MUTED_GAIN_DB);

  if (system_gain_db_ == gain_db) {
    // This system gain is the same as the last one we broadcast.
    // A device might have received a SetDeviceGain call since we last set this.
    // Only update devices that have diverged from the System Gain/Mute values.
    device_manager_.OnSystemGain(false);
    return;
  }

  system_gain_db_ = gain_db;

  // This will be broadcast to all output devices.
  device_manager_.OnSystemGain(true);
  NotifyGainMuteChanged();
}

void AudioCoreImpl::SetSystemMute(bool muted) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetSystemMute");
  AUD_VLOG(TRACE) << " (mute: " << muted << ")";
  if (system_muted_ == muted) {
    // A device might have received a SetDeviceMute call since we last set this.
    // Only update devices that have diverged from the System Gain/Mute values.
    device_manager_.OnSystemGain(false);
    return;
  }

  system_muted_ = muted;

  // This will be broadcast to all output devices.
  device_manager_.OnSystemGain(true);
  NotifyGainMuteChanged();
}

void AudioCoreImpl::NotifyGainMuteChanged() {
  TRACE_DURATION("audio", "AudioCoreImpl::NotifyGainMuteChanged");
  AUD_VLOG(TRACE) << " (" << system_gain_db_ << " dB, mute: " << system_muted_ << ")";
  for (auto& binding : bindings_.bindings()) {
    binding->events().SystemGainMuteChanged(system_gain_db_, system_muted_);
  }
}

float AudioCoreImpl::GetRenderUsageGain(fuchsia::media::AudioRenderUsage usage) {
  TRACE_DURATION("audio", "AudioCoreImpl::GetRenderUsageGain");
  auto usage_index = fidl::ToUnderlying(usage);

  if (usage_index >= fuchsia::media::RENDER_USAGE_COUNT) {
    FXL_LOG(ERROR) << "Unexpected Render Usage: " << usage_index;
    return Gain::kUnityGainDb;
  }
  return Gain::GetRenderUsageGain(usage) + Gain::GetRenderUsageGainAdjustment(usage);
}

float AudioCoreImpl::GetCaptureUsageGain(fuchsia::media::AudioCaptureUsage usage) {
  TRACE_DURATION("audio", "AudioCoreImpl::GetCaptureUsageGain");
  auto usage_index = fidl::ToUnderlying(usage);

  if (usage_index >= fuchsia::media::CAPTURE_USAGE_COUNT) {
    FXL_LOG(ERROR) << "Unexpected Capture Usage: " << usage_index;
    return Gain::kUnityGainDb;
  }
  return Gain::GetCaptureUsageGain(usage) + Gain::GetCaptureUsageGainAdjustment(usage);
}

void AudioCoreImpl::SetRenderUsageGain(fuchsia::media::AudioRenderUsage usage, float gain_db) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetRenderUsageGain");
  AUD_VLOG(TRACE) << " (usage: " << static_cast<int>(usage) << ", " << gain_db << " dB)";

  auto usage_index = fidl::ToUnderlying(usage);
  if (usage_index >= fuchsia::media::RENDER_USAGE_COUNT) {
    FXL_LOG(ERROR) << "Unexpected Render Usage: " << usage_index;
    return;
  }
  Gain::SetRenderUsageGain(usage, gain_db);
}

void AudioCoreImpl::SetCaptureUsageGain(fuchsia::media::AudioCaptureUsage usage, float gain_db) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetCaptureUsageGain");
  AUD_VLOG(TRACE) << " (usage: " << static_cast<int>(usage) << ", " << gain_db << " dB)";

  auto usage_index = fidl::ToUnderlying(usage);
  if (usage_index >= fuchsia::media::CAPTURE_USAGE_COUNT) {
    FXL_LOG(ERROR) << "Unexpected Capture Usage: " << usage_index;
    return;
  }
  Gain::SetCaptureUsageGain(usage, gain_db);
}

void AudioCoreImpl::SetRenderUsageGainAdjustment(fuchsia::media::AudioRenderUsage usage,
                                                 float db_gain) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetRenderUsageGainAdjustment");
  auto usage_index = fidl::ToUnderlying(usage);
  if (usage_index >= fuchsia::media::RENDER_USAGE_COUNT) {
    FXL_LOG(ERROR) << "Unexpected Render Usage: " << usage_index;
    return;
  }
  Gain::SetRenderUsageGainAdjustment(usage, db_gain);
}

void AudioCoreImpl::SetCaptureUsageGainAdjustment(fuchsia::media::AudioCaptureUsage usage,
                                                  float db_gain) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetCaptureUsageGainAdjustment");
  auto usage_index = fidl::ToUnderlying(usage);
  if (usage_index >= fuchsia::media::CAPTURE_USAGE_COUNT) {
    FXL_LOG(ERROR) << "Unexpected Capture Usage: " << usage_index;
    return;
  }
  Gain::SetCaptureUsageGainAdjustment(usage, db_gain);
}

void AudioCoreImpl::SetRoutingPolicy(fuchsia::media::AudioOutputRoutingPolicy policy) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetRoutingPolicy");
  AUD_VLOG(TRACE) << " (policy: " << static_cast<int>(policy) << ")";
  device_manager_.SetRoutingPolicy(policy);
}

void AudioCoreImpl::EnableDeviceSettings(bool enabled) {
  TRACE_DURATION("audio", "AudioCoreImpl::EnableDeviceSettings");
  AUD_VLOG(TRACE) << " (enabled: " << enabled << ")";
  AudioDeviceSettings::EnableDeviceSettings(enabled);
}

void AudioCoreImpl::SetInteraction(fuchsia::media::Usage active, fuchsia::media::Usage affected,
                                   fuchsia::media::Behavior behavior) {
  TRACE_DURATION("audio", "AudioCoreImpl::SetInteraction");
  audio_admin_.SetInteraction(std::move(active), std::move(affected), behavior);
}

void AudioCoreImpl::LoadDefaults() {
  TRACE_DURATION("audio", "AudioCoreImpl::LoadDefaults");
  audio_admin_.LoadDefaults();
}

void AudioCoreImpl::ResetInteractions() {
  TRACE_DURATION("audio", "AudioCoreImpl::ResetInteractions");
  audio_admin_.ResetInteractions();
}

void AudioCoreImpl::UpdateRendererState(fuchsia::media::AudioRenderUsage usage, bool active,
                                        fuchsia::media::AudioRenderer* renderer) {
  TRACE_DURATION("audio", "AudioCoreImpl::UpdateRendererState");
  audio_admin_.UpdateRendererState(usage, active, renderer);
}

void AudioCoreImpl::UpdateCapturerState(fuchsia::media::AudioCaptureUsage usage, bool active,
                                        fuchsia::media::AudioCapturer* capturer) {
  TRACE_DURATION("audio", "AudioCoreImpl::UpdateCapturerState");
  audio_admin_.UpdateCapturerState(usage, active, capturer);
}

void AudioCoreImpl::DoPacketCleanup(trace_async_id_t nonce) {
  TRACE_DURATION("audio", "AudioCoreImpl::DoPacketCleanup");
  TRACE_FLOW_END("audio", "DoPacketCleanup", nonce);
  // In order to minimize the time we spend in the lock we obtain the lock, swap // the contents of
  // the cleanup queue with a local queue and clear the sched flag, and finally unlock clean out the
  // queue (which has the side effect of triggering all of the send packet callbacks).
  //
  // Note: this is only safe because we know that we are executing on a single
  // threaded task runner.  Without this guarantee, it might be possible call
  // the send packet callbacks in a different order than the packets were sent
  // in the first place.  If the async object for the audio service ever loses
  // this serialization guarantee (because it becomes multi-threaded, for
  // example) we will need to introduce another lock (different from the cleanup
  // lock) in order to keep the cleanup tasks properly ordered while
  // guaranteeing minimal contention of the cleanup lock (which is being
  // acquired by the high priority mixing threads).
  fbl::DoublyLinkedList<std::unique_ptr<AudioPacketRef>> tmp_packet_queue;
  fbl::DoublyLinkedList<std::unique_ptr<PendingFlushToken>> tmp_token_queue;

  {
    std::lock_guard<std::mutex> locker(cleanup_queue_mutex_);
    packet_cleanup_queue_.swap(tmp_packet_queue);
    flush_cleanup_queue_.swap(tmp_token_queue);
    cleanup_scheduled_ = false;
  }

  // Call the Cleanup method for each of the packets in order, then let the tmp
  // queue go out of scope cleaning up all of the packet references.
  for (auto& packet_ref : tmp_packet_queue) {
    packet_ref.Cleanup();
  }

  for (auto& token : tmp_token_queue) {
    token.Cleanup();
  }
}

void AudioCoreImpl::SchedulePacketCleanup(std::unique_ptr<AudioPacketRef> packet) {
  TRACE_DURATION("audio", "AudioCoreImpl::SchedulePacketCleanup");
  std::lock_guard<std::mutex> locker(cleanup_queue_mutex_);

  packet_cleanup_queue_.push_back(std::move(packet));

  if (!cleanup_scheduled_ && !shutting_down_) {
    FXL_DCHECK(dispatcher_);
    auto nonce = TRACE_NONCE();
    TRACE_FLOW_BEGIN("audio", "DoPacketCleanup", nonce);
    async::PostTask(dispatcher_, [this, nonce]() { DoPacketCleanup(nonce); });
    cleanup_scheduled_ = true;
  }
}

void AudioCoreImpl::ScheduleFlushCleanup(std::unique_ptr<PendingFlushToken> token) {
  TRACE_DURATION("audio", "AudioCoreImpl::ScheduleFlushCleanup");
  std::lock_guard<std::mutex> locker(cleanup_queue_mutex_);

  flush_cleanup_queue_.push_back(std::move(token));

  if (!cleanup_scheduled_ && !shutting_down_) {
    FXL_DCHECK(dispatcher_);
    auto nonce = TRACE_NONCE();
    TRACE_FLOW_BEGIN("audio", "DoPacketCleanup", nonce);
    async::PostTask(dispatcher_, [this, nonce]() { DoPacketCleanup(nonce); });
    cleanup_scheduled_ = true;
  }
}

}  // namespace media::audio
