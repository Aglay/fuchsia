// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/policy_loader.h"

#include <lib/gtest/test_loop_fixture.h>

#include "src/media/audio/audio_core/policy_loader_unittest_data.h"

namespace media::audio {
namespace {

class AudioAdminUnitTest : public gtest::TestLoopFixture {};

TEST_F(AudioAdminUnitTest, GoodConfigs) {
  // Explicitly passing no rules is an acceptable configuration.
  EXPECT_TRUE(PolicyLoader::ParseConfig(test::empty_rules_json));

  EXPECT_TRUE(PolicyLoader::ParseConfig(test::ignored_key));

  // Test each possible combination of render and capture usage.
  EXPECT_TRUE(PolicyLoader::ParseConfig(test::render_render));
  EXPECT_TRUE(PolicyLoader::ParseConfig(test::render_capture));
  EXPECT_TRUE(PolicyLoader::ParseConfig(test::capture_render));
  EXPECT_TRUE(PolicyLoader::ParseConfig(test::capture_capture));

  // Test a config that contains all possible usage and behavior types.
  EXPECT_TRUE(PolicyLoader::ParseConfig(test::contains_all_usages_and_behaviors));
}

TEST_F(AudioAdminUnitTest, BadConfigs) {
  // Configs that aren't complete enough to use.
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::no_rules));
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::no_active));
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::no_affected));
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::no_behavior));

  // Malformed configs.
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::rules_not_array));
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::rules_array_not_rules));

  // Configs that have all the required parts, but have invalid values.
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::invalid_renderusage));
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::invalid_captureusage));
  EXPECT_FALSE(PolicyLoader::ParseConfig(test::invalid_behavior));
}

}  // namespace
}  // namespace media::audio
