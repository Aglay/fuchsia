// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ostream>

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/component/cpp/environment_services_helper.h>
#include <lib/escher/test/gtest_vulkan.h>
#include <zircon/errors.h>

#include "garnet/public/lib/fostr/fidl/fuchsia/feedback/formatting.h"
#include "src/developer/feedback_agent/annotations.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace fuchsia {
namespace feedback {
namespace {

// Returns true if gMock |arg|.key matches |expected_key|.
MATCHER_P(MatchesKey, expected_key,
          "matches an annotation with key \"" + std::string(expected_key) +
              "\"") {
  return arg.key == expected_key;
}

// Smoke-tests the real environment service for the
// fuchsia.feedback.DataProvider FIDL interface, connecting through FIDL.
class FeedbackAgentIntegrationTest : public testing::Test {
 public:
  void SetUp() override {
    auto environment_services_ = component::GetEnvironmentServices();
    environment_services_->ConnectToService(
        feedback_data_provider_.NewRequest());
  }

  void TearDown() override { feedback_data_provider_.Unbind(); }

 protected:
  DataProviderSyncPtr feedback_data_provider_;

 private:
  std::shared_ptr<component::Services> environment_services_;
};

// We use VK_TEST instead of the regular TEST macro because Scenic needs Vulkan
// to operate properly and take a screenshot. Note that calls to Scenic hang
// indefinitely for headless devices so this test assumes the device has a
// display like the other Scenic tests, see SCN-1281.
VK_TEST_F(FeedbackAgentIntegrationTest, GetScreenshot_SmokeTest) {
  std::unique_ptr<Screenshot> out_screenshot;
  ASSERT_EQ(feedback_data_provider_->GetScreenshot(ImageEncoding::PNG,
                                                   &out_screenshot),
            ZX_OK);
  // We cannot expect a particular payload in the response because Scenic might
  // return a screenshot or not depending on which device the test runs.
}

TEST_F(FeedbackAgentIntegrationTest, GetData_CheckKeys) {
  DataProvider_GetData_Result out_result;
  ASSERT_EQ(feedback_data_provider_->GetData(&out_result), ZX_OK);
  ASSERT_TRUE(out_result.is_response());

  // We cannot expect a particular value for each annotation or attachment
  // because values might depend on which device the test runs (e.g., board
  // name) or what happened prior to running this test (e.g., logs). But we
  // should expect the keys to be present.
  ASSERT_TRUE(out_result.response().data.has_annotations());
  EXPECT_THAT(out_result.response().data.annotations(),
              testing::UnorderedElementsAreArray({
                  MatchesKey("device.board-name"),
                  MatchesKey("build.last-update"),
                  MatchesKey("build.version"),
                  MatchesKey("build.board"),
                  MatchesKey("build.product"),
              }));
  ASSERT_TRUE(out_result.response().data.has_attachments());
  EXPECT_THAT(out_result.response().data.attachments(),
              testing::UnorderedElementsAreArray({
                  MatchesKey("build.snapshot"),
              }));
}

}  // namespace

// Pretty-prints Annotation in gTest matchers instead of the default byte string
// in case of failed expectations.
void PrintTo(const Annotation annotation, std::ostream* os) {
  *os << annotation;
}

}  // namespace feedback
}  // namespace fuchsia
