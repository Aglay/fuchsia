// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of the DeviceShell service that passes a command line
// configurable user name to its UserProvider, and is able to run a story with a
// single module through its life cycle.

#include <memory>
#include <utility>

#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/device/fidl/device_shell.fidl.h"
#include "lib/device/fidl/user_provider.fidl.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/fidl/single_service_app.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

namespace {

class Settings {
 public:
  explicit Settings(const fxl::CommandLine& command_line) {
    // device_name will be set to the device's hostname if it is empty or null
    device_name = command_line.GetOptionValueWithDefault("device_name", "");

    // default user is incognito
    user = command_line.GetOptionValueWithDefault("user", "");

    // If passed, runs as a test harness.
    test = command_line.HasOption("test");
  }

  std::string device_name;
  std::string user;
  bool test{};
};

class DevDeviceShellApp : modular::SingleServiceApp<modular::DeviceShell>,
                          modular::UserWatcher {
 public:
  explicit DevDeviceShellApp(
      component::ApplicationContext* const application_context,
      Settings settings)
      : SingleServiceApp(application_context),
        settings_(std::move(settings)),
        user_watcher_binding_(this) {
    if (settings_.test) {
      modular::testing::Init(this->application_context(), __FILE__);
    }
  }

  ~DevDeviceShellApp() override = default;

  // |SingleServiceApp|
  void Terminate(std::function<void()> done) override {
    if (settings_.test) {
      modular::testing::Teardown(done);
    } else {
      done();
    }
  }

 private:
  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      f1dl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    view_owner_request_ = std::move(view_owner_request);
    Connect();
  }

  // |DeviceShell|
  void Initialize(
      f1dl::InterfaceHandle<modular::DeviceShellContext> device_shell_context,
      modular::DeviceShellParamsPtr device_shell_params) override {
    device_shell_context_.Bind(std::move(device_shell_context));
    device_shell_context_->GetUserProvider(user_provider_.NewRequest());

    Connect();
  }

  // |DeviceShell|
  void GetAuthenticationContext(
      const f1dl::StringPtr& /*username*/,
      f1dl::InterfaceRequest<modular::AuthenticationContext> /*request*/)
      override {
    FXL_LOG(INFO)
        << "DeviceShell::GetAuthenticationContext() is unimplemented.";
  }

  // |UserWatcher|
  void OnLogout() override {
    FXL_LOG(INFO) << "UserWatcher::OnLogout()";
    device_shell_context_->Shutdown();
  }

  void Login(const std::string& account_id) {
    auto params = modular::UserLoginParams::New();
    params->account_id = account_id;
    params->view_owner = std::move(view_owner_request_);
    params->user_controller = user_controller_.NewRequest();
    user_provider_->Login(std::move(params));
    user_controller_->Watch(user_watcher_binding_.NewBinding());
  }

  void Connect() {
    if (user_provider_ && view_owner_request_) {
      if (settings_.user.empty()) {
        // Incognito mode.
        Login("");
        return;
      }

      user_provider_->PreviousUsers(
          [this](f1dl::VectorPtr<modular::auth::AccountPtr> accounts) {
            FXL_LOG(INFO) << "Found " << accounts->size()
                          << " users in the user "
                          << "database";

            // Not running in incognito mode. Add the user if not already
            // added.
            std::string account_id;
            for (const auto& account : *accounts) {
              FXL_LOG(INFO) << "Found user " << account->display_name;
              if (account->display_name->size() >= settings_.user.size() &&
                  account->display_name->substr(settings_.user.size()) ==
                      settings_.user) {
                account_id = account->id;
                break;
              }
            }
            if (account_id.empty()) {
              user_provider_->AddUser(
                  modular::auth::IdentityProvider::DEV,
                  [this](modular::auth::AccountPtr account,
                         const f1dl::StringPtr& status) { Login(account->id); });
            } else {
              Login(account_id);
            }
          });
    }
  }

  const Settings settings_;
  f1dl::Binding<modular::UserWatcher> user_watcher_binding_;
  fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request_;
  modular::DeviceShellContextPtr device_shell_context_;
  modular::UserControllerPtr user_controller_;
  modular::UserProviderPtr user_provider_;
  FXL_DISALLOW_COPY_AND_ASSIGN(DevDeviceShellApp);
};

}  // namespace

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  Settings settings(command_line);

  fsl::MessageLoop loop;

  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<DevDeviceShellApp> driver(
      app_context->outgoing_services(),
      std::make_unique<DevDeviceShellApp>(app_context.get(), settings),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
