// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/device_runner/user_controller_impl.h"

#include <fuchsia/sys/cpp/fidl.h>
#include "gtest/gtest.h"
#include "lib/gtest/test_with_message_loop.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/testing/fake_launcher.h"

namespace fuchsia {
namespace modular {
namespace testing {
namespace {

class UserControllerImplTest : public gtest::TestWithMessageLoop {};

TEST_F(UserControllerImplTest, StartUserRunner) {
  FakeLauncher launcher;
  std::string url = "test_url_string";
  AppConfig app_config;
  app_config.url = url;

  fuchsia::modular::auth::TokenProviderFactoryPtr token_provider_factory_ptr;
  auto token_provider_factory_request = token_provider_factory_ptr.NewRequest();

  bool callback_called = false;
  launcher.RegisterApplication(
      url, [&callback_called](
               fuchsia::sys::LaunchInfo launch_info,
               fidl::InterfaceRequest<fuchsia::sys::ComponentController> ctrl) {
        callback_called = true;
      });

  UserControllerPtr user_controller_ptr;
  UserControllerImpl impl(
      &launcher, CloneStruct(app_config), CloneStruct(app_config),
      CloneStruct(app_config), std::move(token_provider_factory_ptr),
      nullptr /* account */, nullptr /* view_owner_request */,
      nullptr /* device_shell_services */, user_controller_ptr.NewRequest(),
      nullptr /* done_callback */);

  EXPECT_TRUE(callback_called);
}

}  // namespace
}  // namespace testing
}  // namespace modular
}  // namespace fuchsia
