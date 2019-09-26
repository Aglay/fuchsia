// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/crashpad_agent/privacy_settings_ptr.h"

#include <lib/fit/result.h>
#include <lib/fostr/fidl/fuchsia/settings/formatting.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/syslog/cpp/logger.h>
#include <zircon/errors.h>

#include <memory>
#include <optional>

#include "fuchsia/settings/cpp/fidl.h"
#include "src/developer/feedback/crashpad_agent/settings.h"
#include "src/developer/feedback/crashpad_agent/tests/fake_privacy_settings.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/test/test_settings.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

using fuchsia::settings::Error;
using fuchsia::settings::PrivacySettings;

constexpr Settings::UploadPolicy kDisabled = Settings::UploadPolicy::DISABLED;
constexpr Settings::UploadPolicy kEnabled = Settings::UploadPolicy::ENABLED;
constexpr Settings::UploadPolicy kLimbo = Settings::UploadPolicy::LIMBO;

constexpr bool kUserOptIn = true;
constexpr bool kUserOptOut = false;
constexpr std::optional<bool> kNotSet = std::nullopt;

PrivacySettings MakePrivacySettings(const std::optional<bool> user_data_sharing_consent) {
  PrivacySettings settings;
  if (user_data_sharing_consent.has_value()) {
    settings.set_user_data_sharing_consent(user_data_sharing_consent.value());
  }
  return settings;
}

class PrivacySettingsWatcherTest : public gtest::TestLoopFixture,
                                   public testing::WithParamInterface<Settings::UploadPolicy> {
 public:
  PrivacySettingsWatcherTest()
      : service_directory_provider_(dispatcher()),
        watcher_(service_directory_provider_.service_directory(), &crash_reporter_settings_) {}

 protected:
  void ResetPrivacySettings(std::unique_ptr<FakePrivacySettings> fake_privacy_settings) {
    fake_privacy_settings_ = std::move(fake_privacy_settings);
    if (fake_privacy_settings_) {
      FXL_CHECK(service_directory_provider_.AddService(fake_privacy_settings_->GetHandler()) ==
                ZX_OK);
    }
  }

  void SetPrivacySettings(std::optional<bool> user_data_sharing_consent) {
    fit::result<void, Error> set_result;
    fake_privacy_settings_->Set(
        MakePrivacySettings(user_data_sharing_consent),
        [&set_result](fit::result<void, Error> result) { set_result = std::move(result); });
    EXPECT_TRUE(set_result.is_ok());
  }

  void SetInitialUploadPolicy(const Settings::UploadPolicy upload_policy) {
    crash_reporter_settings_.set_upload_policy(upload_policy);
  }

 private:
  sys::testing::ServiceDirectoryProvider service_directory_provider_;

 protected:
  Settings crash_reporter_settings_;
  PrivacySettingsWatcher watcher_;

 private:
  std::unique_ptr<FakePrivacySettings> fake_privacy_settings_;
};

TEST_F(PrivacySettingsWatcherTest, SetUp) {
  EXPECT_TRUE(watcher_.privacy_settings().IsEmpty());
  EXPECT_FALSE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
}

// This allows us to see meaningful names rather than /0, /1 and /2 in the parameterized test case
// names.
std::string PrettyPrintUploadPolicyUploadsEnabledValue(
    const testing::TestParamInfo<Settings::UploadPolicy>& info) {
  switch (info.param) {
    case Settings::UploadPolicy::DISABLED:
      return "DisabledInitially";
    case Settings::UploadPolicy::ENABLED:
      return "EnabledInitially";
    case Settings::UploadPolicy::LIMBO:
      return "LimboInitially";
  }
};

// We want to make sure that regardless of the state in which the crash reporter's upload policy
// started in, the expectations are always the same. In particular that failure paths always end up
// setting the upload policy to LIMBO.
//
// We use a parameterized gTest where the 3 values represent the 3 possible UploadPolicy.
INSTANTIATE_TEST_SUITE_P(WithVariousInitialUploadPolicies, PrivacySettingsWatcherTest,
                         ::testing::ValuesIn(std::vector<Settings::UploadPolicy>({
                             Settings::UploadPolicy::DISABLED,
                             Settings::UploadPolicy::ENABLED,
                             Settings::UploadPolicy::LIMBO,
                         })),
                         &PrettyPrintUploadPolicyUploadsEnabledValue);

TEST_P(PrivacySettingsWatcherTest, UploadPolicyDefaultToDisabledIfServerNotAvailable) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(nullptr);

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_FALSE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_TRUE(watcher_.privacy_settings().IsEmpty());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicyDefaultToDisabledIfServerClosesConnection) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettingsClosesConnection>());

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_FALSE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_TRUE(watcher_.privacy_settings().IsEmpty());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicyDefaultToDisabledIfNoCallToSet) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnFirstWatch_OptIn) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  SetPrivacySettings(kUserOptIn);
  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kEnabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnFirstWatch_OptOut) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  SetPrivacySettings(kUserOptOut);
  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kDisabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnFirstWatch_NotSet) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  SetPrivacySettings(kNotSet);
  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnSecondWatch_OptIn) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptIn);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kEnabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnSecondWatch_OptOut) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptOut);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kDisabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnSecondWatch_NotSet) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kNotSet);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

TEST_P(PrivacySettingsWatcherTest, UploadPolicySwitchesToSetValueOnEachWatch) {
  SetInitialUploadPolicy(GetParam());
  ResetPrivacySettings(std::make_unique<FakePrivacySettings>());

  watcher_.StartWatching();
  RunLoopUntilIdle();
  EXPECT_TRUE(watcher_.IsConnected());
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptIn);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kEnabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptOut);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kDisabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptIn);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kEnabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptIn);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kEnabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptOut);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kDisabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kNotSet);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kUserOptIn);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kEnabled);
  EXPECT_TRUE(watcher_.privacy_settings().has_user_data_sharing_consent());

  SetPrivacySettings(kNotSet);
  RunLoopUntilIdle();
  EXPECT_EQ(crash_reporter_settings_.upload_policy(), kLimbo);
  EXPECT_FALSE(watcher_.privacy_settings().has_user_data_sharing_consent());
}

}  // namespace

// Pretty-prints Settings::UploadPolicy in gTest matchers instead of the default byte
// string in case of failed expectations.
void PrintTo(const Settings::UploadPolicy& upload_policy, std::ostream* os) {
  *os << ToString(upload_policy);
}

}  // namespace feedback

int main(int argc, char** argv) {
  if (!fxl::SetTestSettings(argc, argv)) {
    return EXIT_FAILURE;
  }

  testing::InitGoogleTest(&argc, argv);
  syslog::InitLogger({"feedback", "test"});
  return RUN_ALL_TESTS();
}
