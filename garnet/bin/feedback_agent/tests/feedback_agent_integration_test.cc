// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/feedback/cpp/fidl.h>
#include <gtest/gtest.h>
#include <lib/component/cpp/environment_services_helper.h>
#include <zircon/errors.h>

namespace fuchsia {
namespace feedback {
namespace {

// Smoke-tests the real environment service for the
// fuchsia.feedback.DataProvider FIDL interface, connecting through FIDL.
TEST(FeedbackAgentIntegrationTest, SmokeTest) {
  DataProviderSyncPtr feedback_data_provider;
  auto environment_services = component::GetEnvironmentServices();
  environment_services->ConnectToService(feedback_data_provider.NewRequest());

  Status out_status;
  std::unique_ptr<PngImage> out_image;
  ASSERT_EQ(feedback_data_provider->GetPngScreenshot(&out_status, &out_image),
            ZX_OK);
  EXPECT_EQ(out_status, Status::UNIMPLEMENTED);
  EXPECT_EQ(out_image, nullptr);
}

}  // namespace
}  // namespace feedback
}  // namespace fuchsia
