// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/lib/test/hermetic_audio_test.h"

#include <sstream>
#include <vector>

#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/media/audio/audio_core/audio_device.h"
#include "src/media/audio/lib/logging/logging.h"
#include "src/media/audio/lib/test/capturer_shim.h"
#include "src/media/audio/lib/test/hermetic_audio_environment.h"
#include "src/media/audio/lib/test/inspect.h"
#include "src/media/audio/lib/test/renderer_shim.h"
#include "src/media/audio/lib/test/test_fixture.h"
#include "src/media/audio/lib/test/virtual_device.h"

namespace media::audio::test {

std::unique_ptr<HermeticAudioEnvironment> HermeticAudioTest::environment_;
fuchsia::virtualaudio::ControlSyncPtr HermeticAudioTest::virtual_audio_control_sync_;

void HermeticAudioTest::SetUpTestSuite() {
  SetUpTestSuiteWithOptions(HermeticAudioEnvironment::Options());
}

void HermeticAudioTest::SetUpTestSuiteWithOptions(HermeticAudioEnvironment::Options options) {
  environment_ = std::make_unique<HermeticAudioEnvironment>(options);
  environment_->ConnectToService(virtual_audio_control_sync_.NewRequest());
  virtual_audio_control_sync_->Enable();

  // Reset inspect ID counters. We start a new audio_core each test suite, but use a global virtual
  // driver across all test suites, so we don't reset the virtual device IDs here.
  internal::capturer_shim_next_inspect_id = 1;
  internal::renderer_shim_next_inspect_id = 1;
}

void HermeticAudioTest::TearDownTestSuite() {
  if (virtual_audio_control_sync_.is_bound()) {
    virtual_audio_control_sync_->Disable();
  }
  environment_ = nullptr;
}

void HermeticAudioTest::SetUp() {
  TestFixture::SetUp();

  environment_->ConnectToService(audio_core_.NewRequest());
  audio_core_.set_error_handler(ErrorHandler());

  environment_->ConnectToService(ultrasound_factory_.NewRequest());
  ultrasound_factory_.set_error_handler(ErrorHandler());

  environment_->ConnectToService(audio_dev_enum_.NewRequest());
  audio_dev_enum_.set_error_handler(ErrorHandler());
  WatchForDeviceArrivals();
}

void HermeticAudioTest::TearDown() {
  // These expectations need to be set on all objects. The simplest way to do
  // that is to set them here, as the final step before expectations are checked.
  if (disallow_underflows_) {
    for (auto& [_, device] : devices_) {
      if (device.output) {
        device.output->expected_inspect_properties().uint_values["underflows"] = 0;
      } else {
        device.input->expected_inspect_properties().uint_values["underflows"] = 0;
      }
    }
    for (auto& r : renderers_) {
      r->expected_inspect_properties().uint_values["underflows"] = 0;
    }
  }

  // Validate inspect metrics.
  auto audio_core_inspect =
      environment_->ReadInspect(HermeticAudioEnvironment::kAudioCoreComponent);
  for (auto& [_, device] : devices_) {
    if (device.output) {
      CheckInspectHierarchy(
          audio_core_inspect,
          {"output devices", fxl::StringPrintf("%03lu", device.output->inspect_id())},
          device.output->expected_inspect_properties());
    } else {
      CheckInspectHierarchy(
          audio_core_inspect,
          {"input devices", fxl::StringPrintf("%03lu", device.input->inspect_id())},
          device.input->expected_inspect_properties());
    }
  }
  for (auto& r : renderers_) {
    CheckInspectHierarchy(audio_core_inspect,
                          {"renderers", fxl::StringPrintf("%lu", r->inspect_id())},
                          r->expected_inspect_properties());
  }
  for (auto& c : capturers_) {
    CheckInspectHierarchy(audio_core_inspect,
                          {"capturers", fxl::StringPrintf("%lu", c->inspect_id())},
                          c->expected_inspect_properties());
  }

  // Remove all components.
  for (auto& [_, device] : devices_) {
    device.output = nullptr;
    device.input = nullptr;
  }
  capturers_.clear();
  renderers_.clear();

  if (audio_dev_enum_.is_bound()) {
    WaitForDeviceDepartures();
  }

  TestFixture::TearDown();
}

void HermeticAudioTest::ExpectCallback() {
  TestFixture::ExpectCallback();

  EXPECT_TRUE(audio_core_.is_bound());
}

void HermeticAudioTest::ExpectDisconnect() {
  TestFixture::ExpectDisconnect();

  EXPECT_TRUE(audio_core_.is_bound());
}

template <fuchsia::media::AudioSampleFormat SampleFormat>
VirtualOutput<SampleFormat>* HermeticAudioTest::CreateOutput(
    const audio_stream_unique_id_t& device_id, Format format, size_t frame_count) {
  FX_CHECK(SampleFormat != fuchsia::media::AudioSampleFormat::UNSIGNED_8)
      << "hardware is not expected to support UNSIGNED_8";
  FX_CHECK(audio_dev_enum_.is_bound());

  auto ptr = std::make_unique<VirtualOutput<SampleFormat>>(
      static_cast<TestFixture*>(this), environment_.get(), device_id, format, frame_count);
  auto out = ptr.get();
  auto id = AudioDevice::UniqueIdToString(device_id);
  devices_[id].output = std::move(ptr);

  // Wait until the device is connected.
  RunLoopUntil([this, id, out]() { return out->Ready() && devices_[id].info != std::nullopt; });

  // Ensure device gain is unity.
  auto& info = devices_[id].info;
  auto is_gain_unity = [info]() {
    return info->gain_info.gain_db == 0.0f &&
           !(info->gain_info.flags & fuchsia::media::AudioGainInfoFlag_Mute);
  };
  if (!is_gain_unity()) {
    fuchsia::media::AudioGainInfo unity = {.gain_db = 0.0f, .flags = 0};
    uint32_t set_flags =
        fuchsia::media::SetAudioGainFlag_GainValid | fuchsia::media::SetAudioGainFlag_MuteValid;
    audio_dev_enum_->SetDeviceGain(info->token_id, unity, set_flags);
    RunLoopUntil(is_gain_unity);
  }

  // Wait for device to become the default.
  RunLoopUntil([this, id]() { return devices_[id].is_default; });
  EXPECT_FALSE(error_occurred_);
  return out;
}

template <fuchsia::media::AudioSampleFormat SampleFormat>
VirtualInput<SampleFormat>* HermeticAudioTest::CreateInput(
    const audio_stream_unique_id_t& device_id, Format format, size_t frame_count) {
  FX_CHECK(SampleFormat != fuchsia::media::AudioSampleFormat::UNSIGNED_8)
      << "hardware is not expected to support UNSIGNED_8";
  FX_CHECK(audio_dev_enum_.is_bound());

  auto ptr = std::make_unique<VirtualInput<SampleFormat>>(
      static_cast<TestFixture*>(this), environment_.get(), device_id, format, frame_count);
  auto out = ptr.get();
  auto id = AudioDevice::UniqueIdToString(device_id);
  devices_[id].input = std::move(ptr);

  // Wait until the device is connected.
  RunLoopUntil([this, out, id]() { return out->Ready() && devices_[id].info != std::nullopt; });
  return out;
}

template <fuchsia::media::AudioSampleFormat SampleFormat>
AudioRendererShim<SampleFormat>* HermeticAudioTest::CreateAudioRenderer(
    Format format, size_t frame_count, fuchsia::media::AudioRenderUsage usage) {
  auto ptr = std::make_unique<AudioRendererShim<SampleFormat>>(
      static_cast<TestFixture*>(this), audio_core_, format, frame_count, usage);
  auto out = ptr.get();
  renderers_.push_back(std::move(ptr));

  // Wait until the renderer is connected.
  RunLoopUntil([this, out]() { return error_occurred_ || (out->GetMinLeadTime() > 0); });
  return out;
}

template <fuchsia::media::AudioSampleFormat SampleFormat>
AudioCapturerShim<SampleFormat>* HermeticAudioTest::CreateAudioCapturer(
    Format format, size_t frame_count, fuchsia::media::AudioCapturerConfiguration config) {
  auto ptr = std::make_unique<AudioCapturerShim<SampleFormat>>(
      static_cast<TestFixture*>(this), audio_core_, format, frame_count, std::move(config));
  auto out = ptr.get();
  capturers_.push_back(std::move(ptr));
  return out;
}

template <fuchsia::media::AudioSampleFormat SampleFormat>
UltrasoundRendererShim<SampleFormat>* HermeticAudioTest::CreateUltrasoundRenderer(
    Format format, size_t frame_count) {
  auto ptr = std::make_unique<UltrasoundRendererShim<SampleFormat>>(
      static_cast<TestFixture*>(this), ultrasound_factory_, format, frame_count);
  auto out = ptr.get();
  renderers_.push_back(std::move(ptr));

  // Wait until the renderer is connected.
  RunLoopUntil([this, out]() { return error_occurred_ || (out->GetMinLeadTime() > 0); });
  return out;
}

template <fuchsia::media::AudioSampleFormat SampleFormat>
UltrasoundCapturerShim<SampleFormat>* HermeticAudioTest::CreateUltrasoundCapturer(
    Format format, size_t frame_count) {
  auto ptr = std::make_unique<UltrasoundCapturerShim<SampleFormat>>(
      static_cast<TestFixture*>(this), ultrasound_factory_, format, frame_count);
  auto out = ptr.get();
  capturers_.push_back(std::move(ptr));
  return out;
}

void HermeticAudioTest::WatchForDeviceArrivals() {
  audio_dev_enum_.events().OnDeviceAdded = [this](fuchsia::media::AudioDeviceInfo info) {
    if (token_to_unique_id_.count(info.token_id) > 0) {
      FAIL() << "Device with token " << info.token_id << " already exists";
    }
    auto id = info.unique_id;
    token_to_unique_id_[info.token_id] = id;
    if (info.is_input) {
      if (!devices_[id].input) {
        ADD_FAILURE() << "Unexpected arrival of input device " << id << ", no such device exists";
      }
      if (devices_[id].info != std::nullopt) {
        ADD_FAILURE() << "Duplicate arrival of input device " << id;
      }
    } else {
      if (!devices_[id].output) {
        ADD_FAILURE() << "Unexpected arrival of output device " << id << ", no such device exists";
      }
      if (devices_[id].info != std::nullopt) {
        ADD_FAILURE() << "Duplicate arrival of output device " << id;
      }
    }
    token_to_unique_id_[info.token_id] = id;
    devices_[id].info = info;
    AUD_VLOG(TRACE) << "Output device (token = " << info.token_id << ", id = " << id
                    << ") has been added";
  };

  audio_dev_enum_.events().OnDeviceRemoved = [this](uint64_t token) {
    if (token_to_unique_id_.count(token) == 0) {
      FAIL() << "Unknown device with token " << token;
    }
    auto id = token_to_unique_id_[token];
    ADD_FAILURE() << "Unexpected removal of device " << id;
  };

  audio_dev_enum_.events().OnDeviceGainChanged = [this](uint64_t token,
                                                        fuchsia::media::AudioGainInfo gain_info) {
    if (token_to_unique_id_.count(token) == 0) {
      FAIL() << "Unknown device with token " << token;
    }
    auto id = token_to_unique_id_[token];
    if (devices_[id].info == std::nullopt) {
      FAIL() << "Device has not been added " << id;
    }
    devices_[id].info->gain_info = gain_info;
    AUD_VLOG(TRACE) << "Our output device (" << id << ") changed gain: " << gain_info.gain_db
                    << " dB, "
                    << ((gain_info.flags & fuchsia::media::AudioGainInfoFlag_Mute) ? "MUTE"
                                                                                   : "UNMUTE");
  };

  audio_dev_enum_.events().OnDefaultDeviceChanged = [this](uint64_t old_default_token,
                                                           uint64_t new_default_token) {
    OnDefaultDeviceChanged(old_default_token, new_default_token);
  };
}

void HermeticAudioTest::WaitForDeviceDepartures() {
  audio_dev_enum_.events().OnDeviceAdded = [](fuchsia::media::AudioDeviceInfo device) {
    ADD_FAILURE() << "Unexpected device " << device.unique_id << " added during shutdown";
  };

  audio_dev_enum_.events().OnDeviceRemoved = [this](uint64_t token) {
    if (token_to_unique_id_.count(token) == 0) {
      FAIL() << "Unknown device with token " << token;
    }
    auto id = token_to_unique_id_[token];
    EXPECT_FALSE(devices_[id].is_removed) << "Duplicate removal of device " << id << " in shutdown";
    EXPECT_FALSE(devices_[id].is_default) << "Device was removed while it was still the default!";
    devices_[id].is_removed = true;
  };

  audio_dev_enum_.events().OnDeviceGainChanged = [](uint64_t device_token,
                                                    fuchsia::media::AudioGainInfo) {
    ADD_FAILURE() << "Unexpected device gain changed (" << device_token << ") during shutdown";
  };

  audio_dev_enum_.events().OnDefaultDeviceChanged = [this](uint64_t old_default_token,
                                                           uint64_t new_default_token) {
    OnDefaultDeviceChanged(old_default_token, new_default_token);
  };

  RunLoopUntil([this]() {
    for (auto& it : devices_) {
      if (!it.second.is_removed) {
        return false;
      }
    }
    return true;
  });

  // Mute events, to avoid flakes from "unbind triggers an event elsewhere".
  audio_dev_enum_.events().OnDeviceAdded = nullptr;
  audio_dev_enum_.events().OnDeviceRemoved = nullptr;
  audio_dev_enum_.events().OnDeviceGainChanged = nullptr;
  audio_dev_enum_.events().OnDefaultDeviceChanged = nullptr;
}

void HermeticAudioTest::OnDefaultDeviceChanged(uint64_t old_default_token,
                                               uint64_t new_default_token) {
  EXPECT_TRUE(old_default_token == 0 || token_to_unique_id_.count(old_default_token) > 0)
      << "Default device changed from unknown device " << old_default_token << " to "
      << new_default_token;

  EXPECT_TRUE(new_default_token == 0 || token_to_unique_id_.count(new_default_token) > 0)
      << "Default device changed from " << old_default_token << " to unknown device "
      << new_default_token;

  AUD_VLOG(TRACE) << "Default output device changed from " << old_default_token << " to "
                  << new_default_token;

  if (old_default_token != 0) {
    auto id = token_to_unique_id_[old_default_token];
    devices_[id].is_default = false;
  }
  if (new_default_token != 0) {
    auto id = token_to_unique_id_[new_default_token];
    devices_[id].is_default = true;
  }
}

fuchsia::media::AudioDeviceEnumeratorPtr HermeticAudioTest::TakeOwnershipOfAudioDeviceEnumerator() {
  FX_CHECK(devices_.empty());
  FX_CHECK(capturers_.empty());
  FX_CHECK(renderers_.empty());

  audio_dev_enum_.events().OnDeviceAdded = nullptr;
  audio_dev_enum_.events().OnDeviceRemoved = nullptr;
  audio_dev_enum_.events().OnDeviceGainChanged = nullptr;
  audio_dev_enum_.events().OnDefaultDeviceChanged = nullptr;

  return std::move(audio_dev_enum_);
}

void HermeticAudioTest::CheckInspectHierarchy(const inspect::Hierarchy& root,
                                              const std::vector<std::string>& path,
                                              const ExpectedInspectProperties& expected) {
  auto path_string = fxl::JoinStrings(path, "/");
  auto h = root.GetByPath(path);
  if (!h) {
    ADD_FAILURE() << "Missing inspect hierarchy for " << path_string;
    return;
  }
  expected.Check(path_string, h->node());
}

// Explicitly instantiate all possible implementations.
#define INSTANTIATE(T)                                                                           \
  template VirtualOutput<T>* HermeticAudioTest::CreateOutput<T>(const audio_stream_unique_id_t&, \
                                                                Format, size_t);                 \
  template VirtualInput<T>* HermeticAudioTest::CreateInput<T>(const audio_stream_unique_id_t&,   \
                                                              Format, size_t);                   \
  template AudioRendererShim<T>* HermeticAudioTest::CreateAudioRenderer<T>(                      \
      Format, size_t, fuchsia::media::AudioRenderUsage);                                         \
  template AudioCapturerShim<T>* HermeticAudioTest::CreateAudioCapturer<T>(                      \
      Format, size_t, fuchsia::media::AudioCapturerConfiguration);                               \
  template UltrasoundRendererShim<T>* HermeticAudioTest::CreateUltrasoundRenderer<T>(Format,     \
                                                                                     size_t);    \
  template UltrasoundCapturerShim<T>* HermeticAudioTest::CreateUltrasoundCapturer<T>(Format,     \
                                                                                     size_t);

INSTANTIATE(fuchsia::media::AudioSampleFormat::UNSIGNED_8)
INSTANTIATE(fuchsia::media::AudioSampleFormat::SIGNED_16)
INSTANTIATE(fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32)
INSTANTIATE(fuchsia::media::AudioSampleFormat::FLOAT)

}  // namespace media::audio::test
