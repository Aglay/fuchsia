// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_device_manager.h"

#include <lib/fit/promise.h>
#include <lib/fit/single_threaded_executor.h>

#include <string>

#include <trace/event.h>

#include "src/media/audio/audio_core/audio_capturer_impl.h"
#include "src/media/audio/audio_core/audio_core_impl.h"
#include "src/media/audio/audio_core/audio_link.h"
#include "src/media/audio/audio_core/audio_plug_detector.h"
#include "src/media/audio/audio_core/audio_renderer_impl.h"
#include "src/media/audio/audio_core/driver_output.h"
#include "src/media/audio/audio_core/reporter.h"
#include "src/media/audio/audio_core/throttle_output.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {

AudioDeviceManager::AudioDeviceManager(ThreadingModel* threading_model,
                                       EffectsLoader* effects_loader,
                                       AudioDeviceSettingsPersistence* device_settings_persistence,
                                       const SystemGainMuteProvider& system_gain_mute)
    : threading_model_(*threading_model),
      system_gain_mute_(system_gain_mute),
      effects_loader_(*effects_loader),
      device_settings_persistence_(*device_settings_persistence) {
  FXL_DCHECK(effects_loader);
  FXL_DCHECK(device_settings_persistence);
  FXL_DCHECK(threading_model);
}

AudioDeviceManager::~AudioDeviceManager() {
  Shutdown();
  FXL_DCHECK(devices_.is_empty());
}

// Configure this admin singleton object to manage audio device instances.
zx_status_t AudioDeviceManager::Init() {
  TRACE_DURATION("audio", "AudioDeviceManager::Init");

  // Instantiate and initialize the default throttle output.
  auto throttle_output = ThrottleOutput::Create(&threading_model(), this);
  if (throttle_output == nullptr) {
    FXL_LOG(ERROR) << "AudioDeviceManager failed to create default throttle output!";
    return ZX_ERR_NO_MEMORY;
  }

  threading_model_.FidlDomain().executor()->schedule_task(
      throttle_output->Startup().or_else([throttle_output](zx_status_t& error) {
        FXL_PLOG(ERROR, error) << "AudioDeviceManager failed to initialize the throttle output";
        return throttle_output->Shutdown();
      }));
  throttle_output_ = std::move(throttle_output);

  // Start monitoring for plug/unplug events of pluggable audio output devices.
  zx_status_t res =
      plug_detector_.Start(fit::bind_member(this, &AudioDeviceManager::AddDeviceByChannel));
  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "AudioDeviceManager failed to start plug detector";
    return res;
  }

  return ZX_OK;
}

// We are no longer managing audio devices, unwind everything.
void AudioDeviceManager::Shutdown() {
  TRACE_DURATION("audio", "AudioDeviceManager::Shutdown");
  // Step #1: Stop monitoring plug/unplug events and cancel any pending settings
  // commit task.  We are shutting down and no longer care about these things.
  plug_detector_.Stop();

  // Step #2: Shut down each active AudioCapturer in the system.
  while (!audio_capturers_.is_empty()) {
    auto audio_capturer = audio_capturers_.pop_front();
    audio_capturer->Shutdown();
  }

  // Step #3: Shut down each active AudioRenderer in the system.
  while (!audio_renderers_.is_empty()) {
    auto audio_renderer = audio_renderers_.pop_front();
    audio_renderer->Shutdown();
  }

  // Step #4: Shut down each device which is waiting for initialization.
  std::vector<fit::promise<void>> device_promises;
  while (!devices_pending_init_.is_empty()) {
    auto device = devices_pending_init_.pop_front();
    device_promises.push_back(device->Shutdown());
  }

  // Step #5: Shut down each currently active device in the system.
  while (!devices_.is_empty()) {
    auto device = devices_.pop_front();
    device_promises.push_back(
        fit::join_promises(device->Shutdown(), device_settings_persistence_.FinalizeSettings(
                                                   *device->device_settings()))
            .and_then([](std::tuple<fit::result<void>, fit::result<void, zx_status_t>>& results)
                          -> fit::result<void> {
              FXL_DCHECK(std::get<0>(results).is_ok());
              if (std::get<1>(results).is_error()) {
                return fit::error();
              }
              return fit::ok();
            }));
  }

  // Step #6: Shut down the throttle output.
  device_promises.push_back(throttle_output_->Shutdown());
  throttle_output_ = nullptr;

  fit::run_single_threaded(fit::join_promise_vector(std::move(device_promises)));
}

void AudioDeviceManager::AddDeviceEnumeratorClient(
    fidl::InterfaceRequest<fuchsia::media::AudioDeviceEnumerator> request) {
  bindings_.AddBinding(this, std::move(request));
}

void AudioDeviceManager::AddDevice(const fbl::RefPtr<AudioDevice>& device) {
  TRACE_DURATION("audio", "AudioDeviceManager::AddDevice");
  FXL_DCHECK(device != nullptr);
  FXL_DCHECK(device != throttle_output_);
  FXL_DCHECK(!device->InContainer());

  threading_model_.FidlDomain().executor()->schedule_task(
      device->Startup()
          .and_then([this, device]() mutable { devices_pending_init_.insert(std::move(device)); })
          .or_else([device](zx_status_t& error) {
            FXL_PLOG(ERROR, error) << "AddDevice failed";
            REP(DeviceStartupFailed(*device));
            device->Shutdown();
          }));
}

void AudioDeviceManager::ActivateDevice(const fbl::RefPtr<AudioDevice>& device) {
  TRACE_DURATION("audio", "AudioDeviceManager::ActivateDevice");
  FXL_DCHECK(device != nullptr);
  FXL_DCHECK(device != throttle_output_);

  // Have we already been removed from the pending list?  If so, the device is
  // already shutting down and there is nothing to be done.
  if (!device->InContainer()) {
    return;
  }

  // TODO(johngro): remove this when system gain is fully deprecated.
  // For now, set each output "device" gain to the "system" gain value.
  if (device->is_output()) {
    UpdateDeviceToSystemGain(device);
  }

  // Determine whether this device's persistent settings are actually unique,
  // or if they collide with another device's unique ID.
  //
  // If these settings are currently unique in the system, attempt to load the
  // persisted settings from disk, or create a new persisted settings file for
  // this device if the file is either absent or corrupt.
  //
  // If these settings are not unique, then copy the settings of the device we
  // conflict with, and use them without persistence. Currently, when device
  // instances conflict, we persist only the first instance's settings.
  fbl::RefPtr<AudioDeviceSettings> settings = device->device_settings();
  FXL_DCHECK(settings != nullptr);
  threading_model_.FidlDomain().executor()->schedule_task(
      device_settings_persistence_.LoadSettings(settings).then(
          [this, device,
           settings = std::move(settings)](fit::result<void, zx_status_t>& result) mutable {
            if (result.is_error()) {
              FXL_PLOG(DFATAL, result.error()) << "Unable to load device settings; "
                                               << "device will not use persisted settings";
            }
            ActivateDeviceWithSettings(std::move(device), std::move(settings));
          }));
}

void AudioDeviceManager::ActivateDeviceWithSettings(fbl::RefPtr<AudioDevice> device,
                                                    fbl::RefPtr<AudioDeviceSettings> settings) {
  if (settings->Ignored()) {
    REP(IgnoringDevice(*device));
    RemoveDevice(device);
    return;
  }

  REP(ActivatingDevice(*device));

  // Move the device over to the set of active devices.
  devices_.insert(devices_pending_init_.erase(*device));
  device->SetActivated();

  // TODO(mpuryear): Create this device instance's EffectsProcessor here?

  // Now that we have our gain settings (restored from disk, cloned from
  // others, or default), reapply them via the device itself.  We do this in
  // order to allow the device the chance to apply its own internal limits,
  // which may not permit the values which had been read from disk.
  //
  // TODO(johngro): Clean this pattern up, it is really awkward.  On the one
  // hand, we would really like the settings to be completely independent from
  // the devices, but on the other hand, there are limits for various settings
  // which may be need imposed by the device's capabilities.
  constexpr uint32_t kAllSetFlags = fuchsia::media::SetAudioGainFlag_GainValid |
                                    fuchsia::media::SetAudioGainFlag_MuteValid |
                                    fuchsia::media::SetAudioGainFlag_AgcValid;
  fuchsia::media::AudioGainInfo gain_info;
  settings->GetGainInfo(&gain_info);
  REP(SettingDeviceGainInfo(*device, gain_info, kAllSetFlags));
  device->SetGainInfo(gain_info, kAllSetFlags);

  // TODO(mpuryear): Configure the EffectsProcessor based on settings, here?

  // Notify interested users of this new device. Check whether this will become
  // the new default device, so we can set 'is_default' in the notification
  // properly. Right now, "default" device is defined simply as last-plugged.
  fuchsia::media::AudioDeviceInfo info;
  device->GetDeviceInfo(&info);

  auto last_plugged = FindLastPlugged(device->type());
  info.is_default = (last_plugged && (last_plugged->token() == device->token()));

  for (auto& client : bindings_.bindings()) {
    client->events().OnDeviceAdded(info);
  }

  // Reconsider our current routing policy now that a new device has arrived.
  if (device->plugged()) {
    zx::time plug_time = device->plug_time();
    OnDevicePlugged(device, plug_time);
  }

  // Check whether the default device has changed; if so, update users.
  UpdateDefaultDevice(device->is_input());
}

void AudioDeviceManager::RemoveDevice(const fbl::RefPtr<AudioDevice>& device) {
  TRACE_DURATION("audio", "AudioDeviceManager::RemoveDevice");
  FXL_DCHECK(device != nullptr);
  FXL_DCHECK(device->is_output() || (device != throttle_output_));

  REP(RemovingDevice(*device));

  // TODO(mpuryear): Considering eliminating this; it may not be needed.
  device->PreventNewLinks();
  device->Unlink();

  if (device->activated()) {
    OnDeviceUnplugged(device, device->plug_time());
  }

  // TODO(mpuryear): Persist any final remaining device-effect settings?

  device->Shutdown();
  device_settings_persistence_.FinalizeSettings(*device->device_settings());

  // TODO(mpuryear): Delete this device instance's EffectsProcessor here?

  if (device->InContainer()) {
    auto& device_set = device->activated() ? devices_ : devices_pending_init_;
    device_set.erase(*device);

    // If device was active: reset the default & notify clients of the removal.
    if (device->activated()) {
      UpdateDefaultDevice(device->is_input());

      for (auto& client : bindings_.bindings()) {
        client->events().OnDeviceRemoved(device->token());
      }
    }
  }
}

void AudioDeviceManager::OnPlugStateChanged(const fbl::RefPtr<AudioDevice>& device, bool plugged,
                                            zx::time plug_time) {
  TRACE_DURATION("audio", "AudioDeviceManager::OnPlugStateChanged");
  FXL_DCHECK(device != nullptr);

  // Update our bookkeeping for device's plug state. If no change, we're done.
  if (!device->UpdatePlugState(plugged, plug_time)) {
    return;
  }

  if (plugged) {
    OnDevicePlugged(device, plug_time);
  } else {
    OnDeviceUnplugged(device, plug_time);
  }

  // Check whether the default device has changed; if so, update users.
  UpdateDefaultDevice(device->is_input());
}

// SetSystemGain or SetSystemMute has been called. 'changed' tells us whether
// the System Gain / Mute values actually changed. If not, only update devices
// that (because of calls to SetDeviceGain) have diverged from System settings.
//
// We update link gains in Device::SetGainInfo rather than here, so that we
// catch changes to device gain coming from SetSystemGain OR SetDeviceGain.
void AudioDeviceManager::OnSystemGain(bool changed) {
  TRACE_DURATION("audio", "AudioDeviceManager::OnSystemGain");
  for (auto& device : devices_) {
    if (device.is_output() && (changed || device.system_gain_dirty)) {
      UpdateDeviceToSystemGain(fbl::RefPtr(&device));
      NotifyDeviceGainChanged(device);
      device.system_gain_dirty = false;
    }
    // We intentionally route System Gain only to Output devices, not Inputs.
    // If needed, we could revisit this in the future.
  }
}

void AudioDeviceManager::GetDevices(GetDevicesCallback cbk) {
  TRACE_DURATION("audio", "AudioDeviceManager::GetDevices");
  std::vector<fuchsia::media::AudioDeviceInfo> ret;

  for (const auto& dev : devices_) {
    if (dev.token() != ZX_KOID_INVALID) {
      fuchsia::media::AudioDeviceInfo info;
      dev.GetDeviceInfo(&info);
      info.is_default =
          (dev.token() == (dev.is_input() ? default_input_token_ : default_output_token_));
      ret.push_back(std::move(info));
    }
  }

  cbk(std::move(ret));
}

void AudioDeviceManager::GetDeviceGain(uint64_t device_token, GetDeviceGainCallback cbk) {
  TRACE_DURATION("audio", "AudioDeviceManager::GetDeviceGain");
  auto dev = devices_.find(device_token);

  fuchsia::media::AudioGainInfo info = {0};
  if (dev.IsValid()) {
    FXL_DCHECK(dev->device_settings() != nullptr);
    dev->device_settings()->GetGainInfo(&info);
    cbk(device_token, info);
  } else {
    cbk(ZX_KOID_INVALID, info);
  }
}

void AudioDeviceManager::SetDeviceGain(uint64_t device_token,
                                       fuchsia::media::AudioGainInfo gain_info,
                                       uint32_t set_flags) {
  TRACE_DURATION("audio", "AudioDeviceManager::SetDeviceGain");
  auto dev = devices_.find(device_token);

  if (!dev.IsValid()) {
    return;
  }
  // SetGainInfo clamps out-of-range values (e.g. +infinity) into the device-
  // allowed gain range. NAN is undefined (signless); handle it here and exit.
  if ((set_flags & fuchsia::media::SetAudioGainFlag_GainValid) && isnan(gain_info.gain_db)) {
    FXL_DLOG(WARNING) << "Invalid device gain " << gain_info.gain_db << " dB -- making no change";
    return;
  }

  dev->system_gain_dirty = true;

  // Change the gain and then report the new settings to our clients.
  REP(SettingDeviceGainInfo(*dev, gain_info, set_flags));
  dev->SetGainInfo(gain_info, set_flags);
  NotifyDeviceGainChanged(*dev);
}

void AudioDeviceManager::GetDefaultInputDevice(GetDefaultInputDeviceCallback cbk) {
  cbk(default_input_token_);
}

void AudioDeviceManager::GetDefaultOutputDevice(GetDefaultOutputDeviceCallback cbk) {
  cbk(default_output_token_);
}

void AudioDeviceManager::SelectOutputsForAudioRenderer(AudioRendererImpl* audio_renderer) {
  TRACE_DURATION("audio", "AudioDeviceManager::SelectOutputsForAudioRenderer");
  FXL_DCHECK(audio_renderer);
  FXL_DCHECK(audio_renderer->format_info_valid());

  // TODO(johngro): Add a way to assert that we are on the message loop thread.

  LinkOutputToAudioRenderer(throttle_output_.get(), audio_renderer);
  fbl::RefPtr<AudioOutput> last_plugged = FindLastPluggedOutput();
  if (last_plugged != nullptr) {
    LinkOutputToAudioRenderer(last_plugged.get(), audio_renderer);
  }

  // Figure out the initial minimum clock lead time requirement.
  audio_renderer->RecomputeMinClockLeadTime();
}

void AudioDeviceManager::LinkOutputToAudioRenderer(AudioOutput* output,
                                                   AudioRendererImpl* audio_renderer) {
  TRACE_DURATION("audio", "AudioDeviceManager::LinkOutputToAudioRenderer");
  FXL_DCHECK(output);
  FXL_DCHECK(audio_renderer);

  // Do not create any links if AudioRenderer's output format is not yet set.
  // Links will be created during SelectOutputsForAudioRenderer when the
  // AudioRenderer format is finally set via AudioRendererImpl::SetStreamType.
  if (!audio_renderer->format_info_valid())
    return;

  fbl::RefPtr<AudioLink> link =
      AudioObject::LinkObjects(fbl::RefPtr(audio_renderer), fbl::RefPtr(output));
  // TODO(johngro): get rid of the throttle output.  See MTWN-52
  if ((link != nullptr) && (output == throttle_output_.get())) {
    FXL_DCHECK(link->source_type() == AudioLink::SourceType::Packet);
    audio_renderer->SetThrottleOutput(
        fbl::RefPtr<AudioLinkPacketSource>::Downcast(std::move(link)));
  }
}

void AudioDeviceManager::AddAudioRenderer(fbl::RefPtr<AudioRendererImpl> audio_renderer) {
  FXL_DCHECK(audio_renderer);
  audio_renderers_.push_back(std::move(audio_renderer));
}

void AudioDeviceManager::RemoveAudioRenderer(AudioRendererImpl* audio_renderer) {
  FXL_DCHECK(audio_renderer != nullptr);
  FXL_DCHECK(audio_renderer->InContainer());
  audio_renderers_.erase(*audio_renderer);
}

void AudioDeviceManager::AddAudioCapturer(const fbl::RefPtr<AudioCapturerImpl>& audio_capturer) {
  TRACE_DURATION("audio", "AudioDeviceManager::AddAudioCapturer");
  FXL_DCHECK(audio_capturer != nullptr);
  FXL_DCHECK(!audio_capturer->InContainer());
  audio_capturers_.push_back(audio_capturer);

  fbl::RefPtr<AudioDevice> source;
  if (audio_capturer->loopback()) {
    source = FindLastPluggedOutput(true);
  } else {
    source = FindLastPluggedInput(true);
  }

  if (source != nullptr) {
    FXL_DCHECK(source->driver() != nullptr);
    auto initial_format = source->driver()->GetSourceFormat();

    if (initial_format) {
      audio_capturer->SetInitialFormat(*initial_format);
    }

    if (source->plugged()) {
      AudioObject::LinkObjects(std::move(source), std::move(audio_capturer));
    }
  }
}

void AudioDeviceManager::RemoveAudioCapturer(AudioCapturerImpl* audio_capturer) {
  TRACE_DURATION("audio", "AudioDeviceManager::RemoveAudioCapturer");
  FXL_DCHECK(audio_capturer != nullptr);
  FXL_DCHECK(audio_capturer->InContainer());
  audio_capturers_.erase(*audio_capturer);
}

fbl::RefPtr<AudioDevice> AudioDeviceManager::FindLastPlugged(AudioObject::Type type,
                                                             bool allow_unplugged) {
  TRACE_DURATION("audio", "AudioDeviceManager::FindLastPlugged");
  FXL_DCHECK((type == AudioObject::Type::Output) || (type == AudioObject::Type::Input));
  AudioDevice* best = nullptr;

  // TODO(johngro): Consider tracking last-plugged times in a fbl::WAVLTree, so
  // this operation becomes O(1). N is pretty low right now, so the benefits do
  // not currently outweigh the complexity of maintaining this index.
  for (auto& obj : devices_) {
    auto& device = static_cast<AudioDevice&>(obj);
    if ((device.type() != type) || device.device_settings()->AutoRoutingDisabled()) {
      continue;
    }

    if ((best == nullptr) || (!best->plugged() && device.plugged()) ||
        ((best->plugged() == device.plugged()) && (best->plug_time() < device.plug_time()))) {
      best = &device;
    }
  }

  FXL_DCHECK((best == nullptr) || (best->type() == type));
  if (!allow_unplugged && best && !best->plugged())
    return nullptr;

  return fbl::RefPtr(best);
}

void AudioDeviceManager::OnDeviceUnplugged(const fbl::RefPtr<AudioDevice>& device,
                                           zx::time plug_time) {
  TRACE_DURATION("audio", "AudioDeviceManager::OnDeviceUnplugged");
  FXL_DCHECK(device);

  // First, see if the device is last-plugged (before updating its plug state).
  bool was_last_plugged = FindLastPlugged(device->type()) == device;

  // Update the device's plug state. If no change, then we are done.
  if (!device->UpdatePlugState(false, plug_time)) {
    return;
  }

  // This device is newly-unplugged. Unlink all its current connections.
  device->Unlink();

  // If the device which was unplugged was not the last plugged device in the
  // system, then there has been no change in who was the last plugged device,
  // and no updates to the routing state are needed.
  if (was_last_plugged) {
    // If removed device was an output, recompute the renderer minimum lead time.
    if (device->is_output()) {
      // This was an output. If applying 'last plugged output' policy, link each
      // AudioRenderer to the most-recently-plugged output (if any). Then do the
      // same for each 'loopback' AudioCapturer. Note: our current (hack)
      // routing policy for inputs is always 'last plugged'.
      FXL_DCHECK(static_cast<AudioOutput*>(device.get()) != throttle_output_.get());

      fbl::RefPtr<AudioOutput> replacement = FindLastPluggedOutput();
      if (replacement) {
        for (auto& audio_renderer : audio_renderers_) {
          LinkOutputToAudioRenderer(replacement.get(), &audio_renderer);
        }

        LinkToAudioCapturers(std::move(replacement));
        for (auto& audio_renderer : audio_renderers_) {
          audio_renderer.RecomputeMinClockLeadTime();
        }
      }
    } else {
      // Removed device was the most-recently-plugged input device. Determine
      // the new most-recently-plugged input (if any remain), and iterate our
      // AudioCapturer list to link each non-loopback AudioCapturer to the new
      // default.
      FXL_DCHECK(device->is_input());

      fbl::RefPtr<AudioInput> replacement = FindLastPluggedInput();
      if (replacement) {
        LinkToAudioCapturers(std::move(replacement));
      }
    }
  }
}

void AudioDeviceManager::OnDevicePlugged(const fbl::RefPtr<AudioDevice>& device,
                                         zx::time plug_time) {
  TRACE_DURATION("audio", "AudioDeviceManager::OnDevicePlugged");
  FXL_DCHECK(device);

  if (device->is_output()) {
    // This new device is an output. Inspect the renderer list and "do the right
    // thing" based on our routing policy. If last-plugged policy, change each
    // renderer to target this device (assuming it IS most-recently-plugged).
    // If all-plugged policy, just add this output to the list.
    //
    // Then, apply last-plugged policy to all capturers with loopback sources.
    // The policy mentioned above currently only pertains to Output Routing.
    fbl::RefPtr<AudioOutput> last_plugged = FindLastPluggedOutput();
    auto output = fbl::RefPtr<AudioOutput>::Downcast(std::move(device));

    if (output == last_plugged) {
      for (auto& unlink_tgt : devices_) {
        if (unlink_tgt.is_output() && (&unlink_tgt != output.get())) {
          unlink_tgt.UnlinkSources();
        }
      }

      for (auto& audio_renderer : audio_renderers_) {
        LinkOutputToAudioRenderer(output.get(), &audio_renderer);

        // If we are adding a new link (regardless of whether we may or may
        // not have removed old links based on the specific active policy)
        // because of an output becoming plugged in, we need to recompute the
        // minimum clock lead time requirement, and perhaps update users as to
        // what it is supposed to be.
        //
        // TODO(johngro) : In theory, this could be optimized.  We don't
        // *technically* need to go over the entire set of links and find the
        // largest minimum lead time requirement if we know (for example) that
        // we just added a link, but didn't remove any.  Right now, we are
        // sticking to the simple approach because we know that N (the total
        // number of outputs an input is linked to) is small, and maintaining
        // optimized/specialized logic for computing this value would start to
        // become a real pain as we start to get more complicated in our
        // approach to policy based routing.
        audio_renderer.RecomputeMinClockLeadTime();
      }

      // 'loopback' AudioCapturers should listen to this output now; unlinks previous output from
      // loopback capturers.
      LinkToAudioCapturers(std::move(output));
    }
  } else {
    FXL_DCHECK(device->is_input());

    fbl::RefPtr<AudioInput> last_plugged = FindLastPluggedInput();
    auto& input = static_cast<AudioInput&>(*device);

    // non-'loopback' AudioCapturers should listen to this input now. Unlinks the previous input
    // from the capturers.
    if (&input == last_plugged.get()) {
      LinkToAudioCapturers(std::move(device));
    }
  }
}

// New device arrived and is the most-recently-plugged.
// * If device is an output, all 'loopback' AudioCapturers should listen to this
// output going forward (it is the default output).
// * If device is an input, then all NON-'loopback' AudioCapturers should listen
// to this input going forward (it is the default input).
void AudioDeviceManager::LinkToAudioCapturers(const fbl::RefPtr<AudioDevice>& device) {
  TRACE_DURATION("audio", "AudioDeviceManager::LinkToAudioCapturers");
  bool link_to_loopbacks = device->is_output();

  for (auto& audio_capturer : audio_capturers_) {
    if (audio_capturer.loopback() == link_to_loopbacks) {
      audio_capturer.UnlinkSources();
      AudioObject::LinkObjects(std::move(device), fbl::RefPtr(&audio_capturer));
    }
  }
}

void AudioDeviceManager::NotifyDeviceGainChanged(const AudioDevice& device) {
  TRACE_DURATION("audio", "AudioDeviceManager::NotifyDeviceGainChanged");
  fuchsia::media::AudioGainInfo info;
  FXL_DCHECK(device.device_settings() != nullptr);
  device.device_settings()->GetGainInfo(&info);

  for (auto& client : bindings_.bindings()) {
    client->events().OnDeviceGainChanged(device.token(), info);
  }
}

void AudioDeviceManager::UpdateDefaultDevice(bool input) {
  TRACE_DURATION("audio", "AudioDeviceManager::UpdateDefaultDevice");
  const auto new_dev =
      FindLastPlugged(input ? AudioObject::Type::Input : AudioObject::Type::Output);
  uint64_t new_id = new_dev ? new_dev->token() : ZX_KOID_INVALID;
  uint64_t& old_id = input ? default_input_token_ : default_output_token_;

  if (old_id != new_id) {
    for (auto& client : bindings_.bindings()) {
      client->events().OnDefaultDeviceChanged(old_id, new_id);
    }
    old_id = new_id;
  }
}

void AudioDeviceManager::UpdateDeviceToSystemGain(const fbl::RefPtr<AudioDevice>& device) {
  TRACE_DURATION("audio", "AudioDeviceManager::UpdateDeviceToSystemGain");
  constexpr uint32_t set_flags =
      fuchsia::media::SetAudioGainFlag_GainValid | fuchsia::media::SetAudioGainFlag_MuteValid;
  fuchsia::media::AudioGainInfo set_cmd = {
      system_gain_mute_.system_gain_db(),
      system_gain_mute_.system_muted() ? fuchsia::media::AudioGainInfoFlag_Mute : 0u};

  FXL_DCHECK(device != nullptr);
  REP(SettingDeviceGainInfo(*device, set_cmd, set_flags));
  device->SetGainInfo(set_cmd, set_flags);
}

void AudioDeviceManager::AddDeviceByChannel(zx::channel device_channel, std::string device_name,
                                            bool is_input) {
  TRACE_DURATION("audio", "AudioDeviceManager::AddDeviceByChannel");
  AUD_VLOG(TRACE) << " adding " << (is_input ? "input" : "output") << " '" << device_name << "'";

  // Hand the stream off to the proper type of class to manage.
  fbl::RefPtr<AudioDevice> new_device;
  if (is_input) {
    new_device = AudioInput::Create(std::move(device_channel), &threading_model(), this);
  } else {
    new_device = DriverOutput::Create(std::move(device_channel), &threading_model(), this);
  }

  if (new_device == nullptr) {
    FXL_LOG(ERROR) << "Failed to instantiate audio " << (is_input ? "input" : "output") << " for '"
                   << device_name << "'";
  }

  REP(AddingDevice(device_name, *new_device));
  AddDevice(std::move(new_device));
}

}  // namespace media::audio
