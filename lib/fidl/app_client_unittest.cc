// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/fidl/app_client.h"

#include "garnet/lib/gtest/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/app/fidl/application_controller.fidl.h"
#include "lib/app/fidl/application_launcher.fidl.h"
#include "peridot/lib/fidl/app_client_unittest.fidl.h"
#include "peridot/lib/testing/fake_application_launcher.h"

namespace modular {
namespace testing {
namespace {

constexpr char kServiceName[] = "service1";
constexpr char kTestUrl[] = "some/test/url";

AppConfigPtr GetTestAppConfig() {
  auto app_config = AppConfig::New();
  app_config->url = kTestUrl;
  return app_config;
}

class TestApplicationController : component::ApplicationController {
 public:
  TestApplicationController() : binding_(this) {}

  void Connect(
      f1dl::InterfaceRequest<component::ApplicationController> request) {
    binding_.Bind(std::move(request));
    binding_.set_error_handler([this] { Kill(); });
  }

  bool killed() { return killed_; }

 private:
  void Kill() override { killed_ = true; }

  void Detach() override {}

  void Wait(const WaitCallback& callback) override {}

  f1dl::Binding<component::ApplicationController> binding_;

  bool killed_{};

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApplicationController);
};

class AppClientTest : public gtest::TestWithMessageLoop {};

TEST_F(AppClientTest, BaseRun_Success) {
  bool callback_called = false;
  FakeApplicationLauncher launcher;
  launcher.RegisterApplication(
      kTestUrl,
      [&callback_called](
          component::ApplicationLaunchInfoPtr launch_info,
          f1dl::InterfaceRequest<component::ApplicationController> ctrl) {
        EXPECT_EQ(kTestUrl, launch_info->url);
        callback_called = true;
      });
  AppClientBase app_client_base(&launcher, GetTestAppConfig());

  EXPECT_TRUE(callback_called);
}

TEST_F(AppClientTest, BaseTerminate_Success) {
  FakeApplicationLauncher launcher;
  TestApplicationController controller;
  bool callback_called = false;
  launcher.RegisterApplication(
      kTestUrl,
      [&callback_called, &controller](
          component::ApplicationLaunchInfoPtr launch_info,
          f1dl::InterfaceRequest<component::ApplicationController> ctrl) {
        EXPECT_EQ(kTestUrl, launch_info->url);
        callback_called = true;
        controller.Connect(std::move(ctrl));
      });

  AppClientBase app_client_base(&launcher, GetTestAppConfig());

  bool app_terminated_callback_called = false;
  app_client_base.Teardown(fxl::TimeDelta::Zero(),
                           [&app_terminated_callback_called] {
                             app_terminated_callback_called = true;
                           });

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(
      RunLoopUntilWithTimeout([&app_terminated_callback_called, &controller] {
        return app_terminated_callback_called && controller.killed();
      }));

  EXPECT_TRUE(controller.killed());
}

TEST_F(AppClientTest, Run_Success) {
  bool callback_called = false;
  FakeApplicationLauncher launcher;
  launcher.RegisterApplication(
      kTestUrl,
      [&callback_called](
          component::ApplicationLaunchInfoPtr launch_info,
          f1dl::InterfaceRequest<component::ApplicationController> ctrl) {
        EXPECT_EQ(kTestUrl, launch_info->url);
        callback_called = true;
      });

  AppClient<test::TerminateService> app_client(&launcher, GetTestAppConfig());

  EXPECT_TRUE(callback_called);
}

TEST_F(AppClientTest, RunWithParams_Success) {
  component::ServiceListPtr additional_services = component::ServiceList::New();
  additional_services->names.push_back(kServiceName);
  // We just need |provider_request| to stay around till the end of this test.
  auto provider_request = additional_services->provider.NewRequest();

  bool callback_called = false;
  FakeApplicationLauncher launcher;
  launcher.RegisterApplication(
      kTestUrl,
      [&callback_called](
          component::ApplicationLaunchInfoPtr launch_info,
          f1dl::InterfaceRequest<component::ApplicationController> ctrl) {
        EXPECT_EQ(kTestUrl, launch_info->url);
        auto additional_services = std::move(launch_info->additional_services);
        EXPECT_FALSE(additional_services.is_null());
        EXPECT_EQ(kServiceName, additional_services->names[0]);
        callback_called = true;
      });

  AppClient<test::TerminateService> app_client(
      &launcher, GetTestAppConfig(), "", std::move(additional_services));

  EXPECT_TRUE(callback_called);
}

}  // namespace
}  // namespace testing
}  // namespace modular
