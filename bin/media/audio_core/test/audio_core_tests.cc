// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/media/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/gtest/real_loop_fixture.h>
#include <cmath>

#include "garnet/bin/media/audio_core/test/audio_core_tests_shared.h"
#include "lib/component/cpp/environment_services_helper.h"
#include "lib/fidl/cpp/synchronous_interface_ptr.h"
#include "lib/fxl/logging.h"

namespace media {
namespace audio {
namespace test {

//
// Tests of the asynchronous Audio interface.
//
class AudioCoreTest : public gtest::RealLoopFixture {
 protected:
  void SetUp() override {
    ::gtest::RealLoopFixture::SetUp();

    environment_services_ = component::GetEnvironmentServices();
    environment_services_->ConnectToService(audio_.NewRequest());
    ASSERT_TRUE(audio_);

    audio_.set_error_handler([this](zx_status_t status) {
      error_occurred_ = true;
      QuitLoop();
    });
  }

  void TearDown() override {
    EXPECT_FALSE(error_occurred_);

    audio_capturer_.Unbind();
    audio_renderer_.Unbind();
    audio_.Unbind();

    ::gtest::RealLoopFixture::TearDown();
  }

  static constexpr float kUnityGainDb = 0.0f;

  // Cache the previous systemwide settings for Gain and Mute, and put the
  // system into a known state as the baseline for gain&mute tests.
  // This is split into a separate method, rather than included in SetUp(),
  // because it is not needed for tests that do not change Gain|Mute.
  void SaveState() {
    audio_.events().SystemGainMuteChanged = [this](float gain_db, bool muted) {
      received_gain_db_ = gain_db;
      received_mute_ = muted;
      QuitLoop();
    };

    // When a client connects to Audio, the system enqueues an action to send
    // the newly-connected client a callback with the systemwide Gain|Mute
    // settings. The system executes this action after the client's currently
    // executing task completes. This means that if a client establishes a
    // connection and then registers a SystemGainMuteChanged callback BEFORE
    // returning, this client will subsequently (once the system gets a chance
    // to run) receive an initial notification of Gain|Mute settings at the time
    // of connection. Conversely, if a client DOES return before registering,
    // even after subsequently registering for the event the client has no way
    // of learning the current Gain|Mute settings until they are changed.
    // Also, in this case, if we RunLoopWithTimeout BEFORE registering for
    // SystemGainMuteChanged events, then later when we look for this event
    // here, we will miss the chance to receive that initial event.
    EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));

    prev_system_gain_db_ = received_gain_db_;
    prev_system_mute_ = received_mute_;

    // Now place system into a known state: unity-gain and unmuted.
    if (prev_system_gain_db_ != kUnityGainDb) {
      audio_->SetSystemGain(kUnityGainDb);
      EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
    }
    if (prev_system_mute_) {
      audio_->SetSystemMute(false);
      EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
    }

    // Once these callbacks arrive, we are primed and ready to test gain|mute.
    EXPECT_EQ(received_gain_db_, kUnityGainDb);
    EXPECT_EQ(received_mute_, false);
  }

  // Testing done; restore the previously-saved systemwide Gain|Mute settings.
  // Also, restore the audio output routing policy (as some tests change this).
  // This is split into a separate method, rather than included in TearDown(),
  // because it is not needed for tests that do not change Gain|Mute or routing.
  void RestoreState() {
    // Don't waste time restoring values, if they are already what we want.
    if (received_gain_db_ != prev_system_gain_db_) {
      audio_->SetSystemGain(prev_system_gain_db_);
      RunLoopWithTimeout(kDurationResponseExpected);
    }

    if (received_mute_ != prev_system_mute_) {
      audio_->SetSystemMute(prev_system_mute_);
      RunLoopWithTimeout(kDurationResponseExpected);
    }

    EXPECT_EQ(received_gain_db_, prev_system_gain_db_);
    EXPECT_EQ(received_mute_, prev_system_mute_);

    // Leave this persistent systemwide setting in the default state!
    audio_->SetRoutingPolicy(
        fuchsia::media::AudioOutputRoutingPolicy::LAST_PLUGGED_OUTPUT);
  }

  std::shared_ptr<component::Services> environment_services_;

  fuchsia::media::AudioPtr audio_;
  fuchsia::media::AudioRendererPtr audio_renderer_;
  fuchsia::media::AudioCapturerPtr audio_capturer_;

  float prev_system_gain_db_;
  bool prev_system_mute_;
  float received_gain_db_;
  bool received_mute_;

  bool error_occurred_ = false;
};

constexpr float AudioCoreTest::kUnityGainDb;

// In some tests below, we allow the message loop to run, so that any
// channel-disconnect that may occur (with subsequent reset) can take effect.
//
// Test creation and interface independence of AudioRenderer.
TEST_F(AudioCoreTest, CreateAudioRenderer) {
  ASSERT_TRUE(audio_);

  // Validate Audio can create AudioRenderer interface.
  audio_->CreateAudioRenderer(audio_renderer_.NewRequest());
  // Give Audio and AudioRenderer interfaces a chance to disconnect if needed.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));
  EXPECT_TRUE(audio_);
  EXPECT_TRUE(audio_renderer_);

  // Validate that Audio persists without AudioRenderer.
  audio_renderer_.Unbind();
  EXPECT_FALSE(audio_renderer_);
  // Give Audio interface a chance to disconnect if it must.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));
  EXPECT_TRUE(audio_);

  // Validate AudioRenderer persists after Audio is unbound.
  audio_->CreateAudioRenderer(audio_renderer_.NewRequest());
  audio_.Unbind();
  EXPECT_FALSE(audio_);
  // Give AudioRenderer interface a chance to disconnect if it must.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));
  EXPECT_TRUE(audio_renderer_);
}

// Test creation and interface independence of AudioCapturer.
TEST_F(AudioCoreTest, CreateAudioCapturer) {
  ASSERT_TRUE(audio_);

  // Validate Audio can create AudioCapturer interface.
  audio_->CreateAudioCapturer(audio_capturer_.NewRequest(), false);
  // Give Audio and AudioCapturer interfaces a chance to disconnect if needed.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));
  EXPECT_TRUE(audio_);
  EXPECT_TRUE(audio_capturer_);

  // Validate that Audio persists without AudioCapturer.
  audio_capturer_.Unbind();
  EXPECT_FALSE(audio_capturer_);
  // Give Audio interface a chance to disconnect if needed.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));
  EXPECT_TRUE(audio_);

  // Validate AudioCapturer persists after Audio is unbound.
  audio_->CreateAudioCapturer(audio_capturer_.NewRequest(), true);
  audio_.Unbind();
  EXPECT_FALSE(audio_);
  // Give AudioCapturer interface a chance to disconnect if needed.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));
  EXPECT_TRUE(audio_capturer_);
}

// Test setting the systemwide Mute.
TEST_F(AudioCoreTest, SetSystemMute_Basic) {
  ASSERT_TRUE(audio_);
  SaveState();  // Sets system Gain to 0.0 dB and Mute to false.

  audio_->SetSystemMute(true);
  // Expect: gain-change callback received; Mute is set, Gain is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, kUnityGainDb);
  EXPECT_TRUE(received_mute_);

  audio_->SetSystemMute(false);
  // Expect: gain-change callback received; Mute is cleared, Gain is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, kUnityGainDb);
  EXPECT_FALSE(received_mute_);

  RestoreState();  // Put that gain back where it came from....
}

// Test setting the systemwide Gain.
TEST_F(AudioCoreTest, SetSystemGain_Basic) {
  ASSERT_TRUE(audio_);
  SaveState();  // Sets system Gain to 0.0 dB and Mute to false.

  audio_->SetSystemGain(-11.0f);
  // Expect: gain-change callback received; Gain is updated, Mute is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, -11.0f);
  EXPECT_FALSE(received_mute_);

  audio_->SetSystemMute(true);
  // Expect: gain-change callback received (Mute is now set).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));

  audio_->SetSystemGain(kUnityGainDb);
  // Expect: gain-change callback received; Gain is updated, Mute is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, kUnityGainDb);
  EXPECT_TRUE(received_mute_);

  RestoreState();
}

// Test the independence of systemwide Gain and Mute. Setting the system Gain to
// -- and away from -- MUTED_GAIN_DB should have no effect on the system Mute.
TEST_F(AudioCoreTest, SetSystemMute_Independence) {
  ASSERT_TRUE(audio_);
  SaveState();  // Sets system Gain to 0.0 dB and Mute to false.

  audio_->SetSystemGain(fuchsia::media::MUTED_GAIN_DB);
  // Expect: callback; Gain is mute-equivalent; Mute is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, fuchsia::media::MUTED_GAIN_DB);
  EXPECT_FALSE(received_mute_);

  audio_->SetSystemMute(true);
  // Expect: callback; Mute is set (despite Gain's MUTED_GAIN_DB value).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, fuchsia::media::MUTED_GAIN_DB);
  EXPECT_TRUE(received_mute_);

  audio_->SetSystemGain(-42.0f);
  // Expect: callback; Gain is no longer MUTED_GAIN_DB, but Mute is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, -42.0f);
  EXPECT_TRUE(received_mute_);

  RestoreState();
}

// Test setting the systemwide Mute to the already-set value.
// In these cases, we should receive no gain|mute callback (should timeout).
// Verify this with permutations that include Mute=true and Gain=MUTED_GAIN_DB.
// 'No callback if no change in Mute' should be the case REGARDLESS of Gain.
// This test relies upon Gain-Mute independence verified by previous test.
TEST_F(AudioCoreTest, SetSystemMute_NoCallbackIfNoChange) {
  ASSERT_TRUE(audio_);
  SaveState();  // Sets system Gain to 0.0 dB and Mute to false.

  audio_->SetSystemMute(true);
  // Expect: gain-change callback received (Mute is now set).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  audio_->SetSystemMute(true);
  // Expect: timeout (no callback); no change to Mute, regardless of Gain.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  audio_->SetSystemGain(fuchsia::media::MUTED_GAIN_DB);
  // Expect: gain-change callback received (even though Mute is set).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, fuchsia::media::MUTED_GAIN_DB);
  EXPECT_TRUE(received_mute_);
  audio_->SetSystemMute(true);
  // Expect: timeout (no callback); no change to Mute, regardless of Gain.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  audio_->SetSystemMute(false);
  // Expect: gain-change callback received; Mute is updated, Gain is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, fuchsia::media::MUTED_GAIN_DB);
  EXPECT_FALSE(received_mute_);
  audio_->SetSystemMute(false);
  // Expect: timeout (no callback); no change to Mute, regardless of Gain.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  audio_->SetSystemGain(kUnityGainDb);
  // Expect: gain-change callback received; Mute is updated, Gain is unchanged.
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  EXPECT_EQ(received_gain_db_, kUnityGainDb);
  EXPECT_FALSE(received_mute_);
  audio_->SetSystemMute(false);
  // Expect: timeout (no callback); no change to Mute, regardless of Gain.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  RestoreState();
}

// Test setting the systemwide Gain to the already-set value.
// In these cases, we should receive no gain|mute callback (should timeout).
// Verify this with permutations that include Mute=true and Gain=MUTED_GAIN_DB.
// 'No callback if no change in Gain' should be the case REGARDLESS of Mute.
// This test relies upon Gain-Mute independence verified by previous test.
TEST_F(AudioCoreTest, SetSystemGain_NoCallbackIfNoChange) {
  ASSERT_TRUE(audio_);
  SaveState();  // Sets system Gain to 0.0 dB and Mute to false.

  // If setting gain to existing value, we should not receive a callback.
  audio_->SetSystemGain(kUnityGainDb);
  // Expect: timeout (no callback); no change to Gain.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  audio_->SetSystemMute(true);
  // Expect: gain-change callback received (Mute is now true).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  audio_->SetSystemGain(kUnityGainDb);
  // Expect: timeout (no callback); no change to Gain, regardlesss of Mute.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  audio_->SetSystemGain(fuchsia::media::MUTED_GAIN_DB);
  // Expect: gain-change callback received (Gain is now MUTED_GAIN_DB).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  audio_->SetSystemGain(fuchsia::media::MUTED_GAIN_DB);
  // Expect: timeout (no callback); no change to Gain, regardlesss of Mute.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  audio_->SetSystemMute(false);
  // Expect: gain-change callback received (Mute is now false).
  EXPECT_FALSE(RunLoopWithTimeout(kDurationResponseExpected));
  audio_->SetSystemGain(fuchsia::media::MUTED_GAIN_DB);
  // Expect: timeout (no callback); no change to Gain, regardlesss of Mute.
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  RestoreState();
}

// Test setting (and re-setting) the audio output routing policy.
TEST_F(AudioCoreTest, SetRoutingPolicy) {
  ASSERT_TRUE(audio_);

  audio_->SetRoutingPolicy(
      fuchsia::media::AudioOutputRoutingPolicy::ALL_PLUGGED_OUTPUTS);
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  // Setting policy again should have no effect.
  audio_->SetRoutingPolicy(
      fuchsia::media::AudioOutputRoutingPolicy::ALL_PLUGGED_OUTPUTS);
  EXPECT_TRUE(RunLoopWithTimeout(kDurationTimeoutExpected));

  RestoreState();
}

//
// Tests of the synchronous AudioSync interface.
//
// We expect the async and sync interfaces to track each other exactly -- any
// behavior otherwise is a bug in core FIDL. These tests were only created to
// better understand how errors manifest themselves when using sync interfaces.
// In short, further testing of the sync interfaces (over and above any testing
// done on the async interfaces) should not be needed.
//
class AudioCoreSyncTest : public gtest::RealLoopFixture {
 protected:
  void SetUp() override {
    ::gtest::RealLoopFixture::SetUp();

    environment_services_ = component::GetEnvironmentServices();
    environment_services_->ConnectToService(audio_.NewRequest());
    ASSERT_TRUE(audio_);
  }

  std::shared_ptr<component::Services> environment_services_;
  fuchsia::media::AudioSyncPtr audio_;
  fuchsia::media::AudioRendererSyncPtr audio_renderer_;
  fuchsia::media::AudioCapturerSyncPtr audio_capturer_;
};

// Test creation and interface independence of AudioRenderer.
TEST_F(AudioCoreSyncTest, CreateAudioRenderer) {
  // Validate Audio can create AudioRenderer interface.
  EXPECT_EQ(ZX_OK, audio_->CreateAudioRenderer(audio_renderer_.NewRequest()));
  EXPECT_TRUE(audio_renderer_);

  // Validate that Audio persists without AudioRenderer.
  audio_renderer_ = nullptr;
  ASSERT_TRUE(audio_);

  // Validate AudioRenderer persists after Audio is unbound.
  EXPECT_EQ(ZX_OK, audio_->CreateAudioRenderer(audio_renderer_.NewRequest()));
  audio_ = nullptr;
  EXPECT_TRUE(audio_renderer_);
}

// Test creation and interface independence of AudioCapturer.
TEST_F(AudioCoreSyncTest, CreateAudioCapturer) {
  // Validate Audio can create AudioCapturer interface.
  EXPECT_EQ(ZX_OK,
            audio_->CreateAudioCapturer(audio_capturer_.NewRequest(), true));
  EXPECT_TRUE(audio_capturer_);

  // Validate that Audio persists without AudioCapturer.
  audio_capturer_ = nullptr;
  ASSERT_TRUE(audio_);

  // Validate AudioCapturer persists after Audio is unbound.
  audio_->CreateAudioCapturer(audio_capturer_.NewRequest(), false);
  audio_ = nullptr;
  EXPECT_TRUE(audio_capturer_);
}

// Test the setting of audio output routing policy.
TEST_F(AudioCoreSyncTest, SetRoutingPolicy) {
  // Validate Audio can set last-plugged routing policy synchronously.
  EXPECT_EQ(ZX_OK,
            audio_->SetRoutingPolicy(
                fuchsia::media::AudioOutputRoutingPolicy::LAST_PLUGGED_OUTPUT));

  // Validate Audio can set all-outputs routing policy synchronously.
  EXPECT_EQ(ZX_OK,
            audio_->SetRoutingPolicy(
                fuchsia::media::AudioOutputRoutingPolicy::ALL_PLUGGED_OUTPUTS));

  // This is a persistent systemwide setting. Leave system in the default state!
  EXPECT_EQ(ZX_OK,
            audio_->SetRoutingPolicy(
                fuchsia::media::AudioOutputRoutingPolicy::LAST_PLUGGED_OUTPUT));
}

// TODO(mpuryear): If we ever add functionality such as parameter parsing,
// relocate the below along with it, to a separate main.cc file.
//
int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();

  return result;
}

}  // namespace test
}  // namespace audio
}  // namespace media
