// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>

#include <cmath>

#include "src/media/audio/lib/test/hermetic_audio_test.h"

namespace media::audio::test {

//
// AudioTest
//
class AudioTest : public HermeticAudioCoreTest {
 protected:
  void TearDown() override;

  fuchsia::media::AudioRendererPtr audio_renderer_;
  fuchsia::media::AudioCapturerPtr audio_capturer_;
};

//
// UsageVolumeControlTest
//
class UsageVolumeControlTest : public HermeticAudioCoreTest {};

//
// AudioTest implementation
//
void AudioTest::TearDown() {
  audio_renderer_.Unbind();
  audio_capturer_.Unbind();

  HermeticAudioCoreTest::TearDown();
}

//
// UsageReporterTest
//
class UsageReporterTest : public HermeticAudioCoreTest {
 protected:
  class FakeUsageWatcher : public fuchsia::media::UsageWatcher {
   public:
    explicit FakeUsageWatcher(fit::closure completer)
        : completer_(std::move(completer)), binding_(this) {}

    fidl::InterfaceHandle<fuchsia::media::UsageWatcher> Bind() { return binding_.NewBinding(); }

   private:
    void OnStateChanged(fuchsia::media::Usage _usage, fuchsia::media::UsageState _usage_state,
                        OnStateChangedCallback callback) override {
      callback();
      completer_();
    }

    fit::closure completer_;
    fidl::Binding<fuchsia::media::UsageWatcher> binding_;
  };
};

//
// Test that the user is connected to the usage reporter.
//
TEST_F(UsageReporterTest, ConnectToUsageReporter) {
  fit::closure completer = CompletionCallback([] {});

  fuchsia::media::UsageReporterPtr audio_core;
  environment()->ConnectToService(audio_core.NewRequest());
  audio_core.set_error_handler(ErrorHandler());

  fuchsia::media::Usage usage;
  usage.set_render_usage(fuchsia::media::AudioRenderUsage::MEDIA);

  FakeUsageWatcher watcher(std::move(completer));
  audio_core->Watch(std::move(usage), watcher.Bind());

  ExpectCallback();
}

//
// UsageGainReporterTest
//
class UsageGainReporterTest : public HermeticAudioCoreTest {
 protected:
  static void SetUpTestSuite() {
    HermeticAudioCoreTest::SetUpTestSuiteWithOptions(HermeticAudioEnvironment::Options{
        .audio_core_config_data_path = "/pkg/data/test_output",
    });
  }
  void SetUp() override {
    HermeticAudioCoreTest::SetUp();
    environment()->ConnectToService(virtualaudio_control_.NewRequest());
    virtualaudio_control_->Enable();
  }
  void TearDown() override {
    // Ensure all devices are now removed
    fuchsia::media::AudioDeviceEnumeratorSyncPtr enumerator;
    environment()->ConnectToService(enumerator.NewRequest());
    RunLoopUntil([&enumerator] {
      std::vector<fuchsia::media::AudioDeviceInfo> devices;
      zx_status_t status = enumerator->GetDevices(&devices);
      FX_CHECK(status == ZX_OK);
      return devices.empty();
    });

    virtualaudio_control_->Disable();
    HermeticAudioCoreTest::TearDown();
  }

  fuchsia::virtualaudio::OutputPtr AddVirtualOutput(
      const std::array<uint8_t, 16>& output_unique_id) {
    fuchsia::media::AudioDeviceEnumeratorPtr enumerator;
    environment()->ConnectToService(enumerator.NewRequest());

    bool device_default = false;
    enumerator.events().OnDefaultDeviceChanged = [&device_default](uint64_t old_default_token,
                                                                   uint64_t new_default_token) {
      device_default = (new_default_token != 0) ? true : false;
    };

    fuchsia::virtualaudio::OutputPtr output;

    bool device_started = false;
    output.events().OnStart = [&device_started](zx_time_t start_time) { device_started = true; };

    environment()->ConnectToService(output.NewRequest());
    output.set_error_handler(ErrorHandler());
    output->SetUniqueId(output_unique_id);
    output->Add();

    RunLoopUntil([&device_started, &device_default] { return device_started & device_default; });

    return output;
  }

  class FakeGainListener : public fuchsia::media::UsageGainListener {
   public:
    explicit FakeGainListener(fit::closure completer)
        : completer_(std::move(completer)), binding_(this) {
      binding_.set_error_handler([](zx_status_t status) { ASSERT_EQ(status, ZX_OK); });
    }

    fidl::InterfaceHandle<fuchsia::media::UsageGainListener> NewBinding() {
      return binding_.NewBinding();
    }

    bool muted() const { return last_muted_; }

    float gain_dbfs() const { return last_gain_dbfs_; }

   private:
    // |fuchsia::media::UsageGainListener|
    void OnGainMuteChanged(bool muted, float gain_dbfs, OnGainMuteChangedCallback callback) final {
      last_muted_ = muted;
      last_gain_dbfs_ = gain_dbfs;
      completer_();
    }

    fit::closure completer_;
    fidl::Binding<fuchsia::media::UsageGainListener> binding_;
    bool last_muted_ = false;
    float last_gain_dbfs_ = 0.0;
  };

  fuchsia::virtualaudio::ControlSyncPtr virtualaudio_control_;

  // This matches the configuration in test_output_audio_core_config.json
  const std::string device_id_string_ = "ffffffffffffffffffffffffffffffff";
  const std::array<uint8_t, 16> device_id_array_ = {{
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
  }};
};

//
// Test that the user is connected to the usage gain reporter.
//
TEST_F(UsageGainReporterTest, ConnectToUsageGainReporter) {
  fit::closure completer = CompletionCallback([] {});

  // Keep output alive for duration of test
  auto output = AddVirtualOutput(device_id_array_);

  fuchsia::media::Usage usage;
  usage.set_render_usage(fuchsia::media::AudioRenderUsage::MEDIA);

  fuchsia::media::audio::VolumeControlPtr volume_control;
  audio_core_->BindUsageVolumeControl(fidl::Clone(usage), volume_control.NewRequest());

  fuchsia::media::UsageGainReporterPtr gain_reporter;
  environment()->ConnectToService(gain_reporter.NewRequest());
  gain_reporter.set_error_handler(ErrorHandler());

  auto fake_listener = std::make_unique<FakeGainListener>(std::move(completer));
  gain_reporter->RegisterListener(device_id_string_, fidl::Clone(usage),
                                  fake_listener->NewBinding());

  volume_control->SetVolume(1.0);
  ExpectCallback();
  EXPECT_FALSE(fake_listener->muted());
  EXPECT_FLOAT_EQ(fake_listener->gain_dbfs(), 0.0);
}

//
// Test that the user is connected to the activity reporter.
//
TEST_F(AudioTest, ConnectToActivityReporter) {
  fuchsia::media::ActivityReporterPtr activity_reporter;
  environment()->ConnectToService(activity_reporter.NewRequest());
  activity_reporter.set_error_handler(ErrorHandler());

  activity_reporter->WatchRenderActivity(
      CompletionCallback([&](const std::vector<fuchsia::media::AudioRenderUsage>& activity) {}));

  ExpectCallback();
}

//
// Audio validation
// Tests of the asynchronous Audio interface.
//
// In some tests below, we run the message loop, so that any channel-disconnect
// from error -- with subsequent reset of the interface ptr -- can take effect.
//
// Test creation and interface independence of AudioRenderer.
// The following 4 conditions are validated:
// 1. Audio can create AudioRenderer.
// 2. Audio persists after created AudioRenderer is destroyed.
// 3. AudioRenderer2 persists after Audio2 is destroyed.
// 4. Asynchronous Audio can create synchronous AudioRenderer, too.
TEST_F(AudioTest, CreateAudioRenderer) {
  audio_core_->CreateAudioRenderer(audio_renderer_.NewRequest());
  audio_renderer_.set_error_handler(ErrorHandler());

  fuchsia::media::AudioRendererSyncPtr audio_renderer_sync;
  audio_core_->CreateAudioRenderer(audio_renderer_sync.NewRequest());

  fuchsia::media::AudioCorePtr audio_core_2;
  environment()->ConnectToService(audio_core_2.NewRequest());
  audio_core_2.set_error_handler(ErrorHandler());

  fuchsia::media::AudioRendererPtr audio_renderer_2;
  audio_core_2->CreateAudioRenderer(audio_renderer_2.NewRequest());
  audio_renderer_2.set_error_handler(ErrorHandler());

  // Before unbinding these, verify they survived this far.
  EXPECT_TRUE(audio_core_2.is_bound());
  audio_core_2.Unbind();

  EXPECT_TRUE(audio_renderer_.is_bound());
  audio_renderer_.Unbind();

  // ...allow them to completely unbind. Will it affect their parent/child?
  audio_renderer_2->GetMinLeadTime(CompletionCallback([](int64_t) {}));
  ExpectCallback();

  // Validate AudioRendererSync was successfully created.
  EXPECT_TRUE(audio_renderer_sync.is_bound());

  // Validate child AudioRenderer2 persists after parent Audio2 was unbound.
  EXPECT_TRUE(audio_renderer_2.is_bound());

  // TearDown will validate that parent Audio survived after child unbound.
}

// Test creation and interface independence of AudioCapturer.
// The following 4 conditions are validated:
// 1. Audio can create AudioCapturer.
// 2. Audio persists after created AudioCapturer is destroyed.
// 3. AudioCapturer2 persists after Audio2 is destroyed.
// 4. Asynchronous Audio can create synchronous AudioCapturer, too.
TEST_F(AudioTest, CreateAudioCapturer) {
  audio_core_->CreateAudioCapturer(false, audio_capturer_.NewRequest());
  audio_capturer_.set_error_handler(ErrorHandler());

  fuchsia::media::AudioCapturerSyncPtr audio_capturer_sync;
  audio_core_->CreateAudioCapturer(false, audio_capturer_sync.NewRequest());

  fuchsia::media::AudioCorePtr audio_core_2;
  environment()->ConnectToService(audio_core_2.NewRequest());
  audio_core_2.set_error_handler(ErrorHandler());

  fuchsia::media::AudioCapturerPtr audio_capturer_2;
  audio_core_2->CreateAudioCapturer(false, audio_capturer_2.NewRequest());
  audio_capturer_2.set_error_handler(ErrorHandler());

  // Before unbinding these, verify they survived this far.
  EXPECT_TRUE(audio_core_2.is_bound());
  audio_core_2.Unbind();

  EXPECT_TRUE(audio_capturer_.is_bound());
  audio_capturer_.Unbind();

  // ...allow them to completely unbind. Will it affect their parent/child?
  audio_capturer_2->GetStreamType(CompletionCallback([](fuchsia::media::StreamType) {}));
  ExpectCallback();

  // Validate AudioCapturerSync was successfully created.
  EXPECT_TRUE(audio_capturer_sync.is_bound());

  // Validate AudioCapturer2 persists after Audio2 was unbound.
  EXPECT_TRUE(audio_capturer_2.is_bound());

  // TearDown will validate that parent Audio survived after child unbound.
}

TEST_F(UsageVolumeControlTest, ConnectToRenderUsageVolume) {
  fuchsia::media::AudioCorePtr audio_core;
  environment()->ConnectToService(audio_core.NewRequest());
  audio_core.set_error_handler(ErrorHandler());

  fuchsia::media::audio::VolumeControlPtr client1;
  fuchsia::media::audio::VolumeControlPtr client2;

  fuchsia::media::Usage usage;
  usage.set_render_usage(fuchsia::media::AudioRenderUsage::MEDIA);

  audio_core->BindUsageVolumeControl(fidl::Clone(usage), client1.NewRequest());
  audio_core->BindUsageVolumeControl(fidl::Clone(usage), client2.NewRequest());

  float volume = 0.0;
  bool muted = false;
  client2.events().OnVolumeMuteChanged =
      CompletionCallback([&volume, &muted](float new_volume, bool new_muted) {
        volume = new_volume;
        muted = new_muted;
      });

  ExpectCallback();
  EXPECT_FLOAT_EQ(volume, 1.0);

  client1->SetVolume(0.5);
  ExpectCallback();
  EXPECT_FLOAT_EQ(volume, 0.5);
  EXPECT_EQ(muted, false);

  client1->SetMute(true);
  ExpectCallback();
  EXPECT_EQ(muted, true);
}

TEST_F(UsageVolumeControlTest, FailToConnectToCaptureUsageVolume) {
  fuchsia::media::Usage usage;
  usage.set_capture_usage(fuchsia::media::AudioCaptureUsage::SYSTEM_AGENT);

  std::optional<zx_status_t> client_error;
  fuchsia::media::audio::VolumeControlPtr client;
  client.set_error_handler([&client_error](zx_status_t status) { client_error = status; });

  audio_core_->BindUsageVolumeControl(fidl::Clone(usage), client.NewRequest());
  RunLoopUntil([&client_error] { return client_error != std::nullopt; });

  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, *client_error);
}

//
// TODO(mpuryear): "fuzz" tests (FIDL-compliant but protocol-inconsistent).
//

}  // namespace media::audio::test
