// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_device.h"

#include <lib/fit/bridge.h>

#include <trace/event.h>

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/audio_link.h"
#include "src/media/audio/audio_core/audio_output.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio {

namespace {
std::string AudioDeviceUniqueIdToString(const audio_stream_unique_id_t& id) {
  static_assert(sizeof(id.data) == 16, "Unexpected unique ID size");
  char buf[(sizeof(id.data) * 2) + 1];

  const auto& d = id.data;
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13],
           d[14], d[15]);
  return std::string(buf, sizeof(buf) - 1);
}
}  // namespace

AudioDevice::AudioDevice(AudioObject::Type type, AudioDeviceManager* manager)
    : AudioObject(type),
      manager_(manager),
      mix_domain_(manager ? manager->threading_model().AcquireMixDomain() : nullptr),
      driver_(new AudioDriver(this)) {
  // audio_core_inspect_tests currently relies on creating a subclass of AudioDevice with a null
  // AudioDeviceManager, so we skip the nullptr asserts on |manager| and |mix_domain_|.
  FXL_DCHECK((type == Type::Input) || (type == Type::Output));
}

AudioDevice::~AudioDevice() = default;

void AudioDevice::Wakeup() {
  TRACE_DURATION("audio", "AudioDevice::Wakeup");
  mix_wakeup_.Signal();
}

std::optional<VolumeCurve> AudioDevice::GetVolumeCurve() const {
  // ThrottleOutput does not have a driver.
  if (!driver_) {
    return std::nullopt;
  }

  // TODO(35394): Add actual curve to this config, store it in driver_ and validate at load time.
  const auto caps = driver_->hw_gain_state();
  if (caps.min_gain == Gain::kUnityGainDb) {
    return std::nullopt;
  }

  return {VolumeCurve::DefaultForMinGain(caps.min_gain)};
}

uint64_t AudioDevice::token() const {
  return driver_ ? driver_->stream_channel_koid() : ZX_KOID_INVALID;
}

// Change a device's gain, propagating the change to the affected links.
void AudioDevice::SetGainInfo(const fuchsia::media::AudioGainInfo& info, uint32_t set_flags) {
  TRACE_DURATION("audio", "AudioDevice::SetGainInfo");
  // Limit the request to what the hardware can support
  fuchsia::media::AudioGainInfo limited = info;
  ApplyGainLimits(&limited, set_flags);

  // For outputs, change the gain of all links where it is the destination.
  if (is_output()) {
    fbl::AutoLock links_lock(&links_lock_);
    for (auto& link : source_links_) {
      if (link.GetSource()->type() == AudioObject::Type::AudioRenderer) {
        link.bookkeeping()->gain.SetDestMute(limited.flags &
                                             fuchsia::media::AudioGainInfoFlag_Mute);
        link.bookkeeping()->gain.SetDestGain(limited.gain_db);
      }
    }
  } else {
    // For inputs, change the gain of all links where it is the source.
    FXL_DCHECK(is_input());
    fbl::AutoLock links_lock(&links_lock_);
    for (auto& link : dest_links_) {
      if (link.GetDest()->type() == AudioObject::Type::AudioCapturer) {
        link.bookkeeping()->gain.SetSourceMute(limited.flags &
                                               fuchsia::media::AudioGainInfoFlag_Mute);
        link.bookkeeping()->gain.SetSourceGain(limited.gain_db);
      }
    }
  }

  FXL_DCHECK(device_settings_ != nullptr);
  if (device_settings_->SetGainInfo(limited, set_flags)) {
    Wakeup();
  }
}

zx_status_t AudioDevice::Init() {
  TRACE_DURATION("audio", "AudioDevice::Init");
  WakeupEvent::ProcessHandler process_handler(
      [output = fbl::RefPtr(this)](WakeupEvent* event) -> zx_status_t {
        OBTAIN_EXECUTION_DOMAIN_TOKEN(token, output->mix_domain_);
        output->OnWakeup();
        return ZX_OK;
      });

  zx_status_t res = mix_wakeup_.Activate(mix_domain_->dispatcher(), std::move(process_handler));
  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to activate wakeup event for AudioDevice";
    return res;
  }

  return ZX_OK;
}

void AudioDevice::Cleanup() {
  TRACE_DURATION("audio", "AudioDevice::Cleanup");
  mix_wakeup_.Deactivate();
  // ThrottleOutput devices have no driver, so check for that.
  if (driver_ != nullptr) {
    // Instruct the driver to release all its resources (channels, timer).
    driver_->Cleanup();
  }
  mix_domain_.reset();
}

void AudioDevice::ActivateSelf() {
  TRACE_DURATION("audio", "AudioDevice::ActivateSelf");
  // If we aren't shutting down, tell DeviceManager we are ready for work.
  if (!is_shutting_down()) {
    // Create default settings. The device manager will restore these settings
    // from persistent storage for us when it gets our activation message.
    FXL_DCHECK(device_settings_ == nullptr);
    FXL_DCHECK(driver() != nullptr);
    device_settings_ = AudioDeviceSettings::Create(*driver(), is_input());

    // Now poke our manager.
    FXL_DCHECK(manager_);
    manager_->ScheduleMainThreadTask([manager = manager_, self = fbl::RefPtr(this)]() {
      manager->ActivateDevice(std::move(self));
    });
  }
}

void AudioDevice::ShutdownSelf() {
  TRACE_DURATION("audio", "AudioDevice::ShutdownSelf");
  // If we are not already in the process of shutting down, send a message to
  // the main message loop telling it to complete the shutdown process.
  if (!is_shutting_down()) {
    shutting_down_.store(true);
    // TODO(mpuryear): Considering eliminating this; it may not be needed.
    PreventNewLinks();

    FXL_DCHECK(manager_);
    manager_->ScheduleMainThreadTask(
        [manager = manager_, self = fbl::RefPtr(this)]() { manager->RemoveDevice(self); });
  }
}

fit::promise<void, zx_status_t> AudioDevice::Startup() {
  TRACE_DURATION("audio", "AudioDevice::Startup");
  fit::bridge<void, zx_status_t> bridge;
  async::PostTask(mix_domain_->dispatcher(), [self = fbl::RefPtr(this),
                                              completer = std::move(bridge.completer)]() mutable {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, self->mix_domain_);
    zx_status_t res = self->Init();
    if (res != ZX_OK) {
      self->Cleanup();
      completer.complete_error(res);
      return;
    }
    self->OnWakeup();
    completer.complete_ok();
  });
  return bridge.consumer.promise();
}

fit::promise<void> AudioDevice::Shutdown() {
  TRACE_DURATION("audio", "AudioDevice::Shutdown");
  // The only reason we have this flag is to make sure that Shutdown is idempotent.
  if (shut_down_) {
    return fit::make_ok_promise();
  }
  shut_down_ = true;

  // Unlink ourselves from everything we are currently attached to.
  Unlink();

  // Give our derived class, and our driver, a chance to clean up resources.
  fit::bridge<void> bridge;
  async::PostTask(mix_domain_->dispatcher(), [self = fbl::RefPtr(this),
                                              completer = std::move(bridge.completer)]() mutable {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, self->mix_domain_);
    self->Cleanup();
    completer.complete_ok();
  });
  return bridge.consumer.promise();
}

bool AudioDevice::UpdatePlugState(bool plugged, zx_time_t plug_time) {
  TRACE_DURATION("audio", "AudioDevice::UpdatePlugState");
  if ((plugged != plugged_) && (plug_time >= plug_time_)) {
    plugged_ = plugged;
    plug_time_ = plug_time;
    return true;
  }

  return false;
}

const fbl::RefPtr<DriverRingBuffer>& AudioDevice::driver_ring_buffer() const {
  return driver_->ring_buffer();
};

const TimelineFunction& AudioDevice::driver_clock_mono_to_ring_pos_bytes() const {
  return driver_->clock_mono_to_ring_pos_bytes();
};

void AudioDevice::GetDeviceInfo(fuchsia::media::AudioDeviceInfo* out_info) const {
  TRACE_DURATION("audio", "AudioDevice::GetDeviceInfo");
  const auto& drv = *driver();
  out_info->name = drv.manufacturer_name() + ' ' + drv.product_name();
  out_info->unique_id = AudioDeviceUniqueIdToString(drv.persistent_unique_id());
  out_info->token_id = token();
  out_info->is_input = is_input();
  out_info->is_default = false;

  FXL_DCHECK(device_settings_);
  device_settings_->GetGainInfo(&out_info->gain_info);
}

}  // namespace media::audio
