// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of the DeviceShell service that passes a command line
// configurable user name to its UserProvider, and is able to run a story with a
// single module through its life cycle.

#include <memory>
#include <utility>

#include <fuchsia/modular/cpp/fidl.h>
#include <views_v1_token/cpp/fidl.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/callback/scoped_callback.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/memory/weak_ptr.h"
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

class DevDeviceShellApp
    : fuchsia::modular::SingleServiceApp<fuchsia::modular::DeviceShell>,
      fuchsia::modular::UserWatcher {
 public:
  explicit DevDeviceShellApp(
      component::ApplicationContext* const application_context,
      Settings settings)
      : SingleServiceApp(application_context),
        settings_(std::move(settings)),
        user_watcher_binding_(this),
        weak_ptr_factory_(this) {
    if (settings_.test) {
      fuchsia::modular::testing::Init(this->application_context(), __FILE__);
      fuchsia::modular::testing::Await(
          fuchsia::modular::testing::kTestShutdown,
          [this] { device_shell_context_->Shutdown(); });

      // Start a timer to quit in case a test component misbehaves and hangs.
      async::PostDelayedTask(
          async_get_default(),
          callback::MakeScoped(weak_ptr_factory_.GetWeakPtr(),
                               [this] {
                                 FXL_LOG(WARNING) << "DevDeviceShell timed out";
                                 device_shell_context_->Shutdown();
                               }),
          zx::msec(fuchsia::modular::testing::kTestTimeoutMilliseconds));
    }
  }

  ~DevDeviceShellApp() override = default;

  // |SingleServiceApp|
  void Terminate(std::function<void()> done) override {
    if (settings_.test) {
      fuchsia::modular::testing::Teardown(done);
    } else {
      done();
    }
  }

 private:
  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      fidl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    view_owner_request_ = std::move(view_owner_request);
    Connect();
  }

  // |DeviceShell|
  void Initialize(
      fidl::InterfaceHandle<fuchsia::modular::DeviceShellContext>
          device_shell_context,
      fuchsia::modular::DeviceShellParams device_shell_params) override {
    device_shell_context_.Bind(std::move(device_shell_context));
    device_shell_context_->GetUserProvider(user_provider_.NewRequest());

    Connect();
  }

  // |DeviceShell|
  void GetAuthenticationContext(
      fidl::StringPtr /*username*/,
      fidl::InterfaceRequest<modular_auth::AuthenticationContext> /*request*/)
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
    fuchsia::modular::UserLoginParams params;
    params.account_id = account_id;
    params.view_owner = std::move(view_owner_request_);
    params.user_controller = user_controller_.NewRequest();
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
          [this](fidl::VectorPtr<modular_auth::Account> accounts) {
            FXL_LOG(INFO) << "Found " << accounts->size()
                          << " users in the user "
                          << "database";

            // Not running in incognito mode. Add the user if not already
            // added.
            std::string account_id;
            for (const auto& account : *accounts) {
              FXL_LOG(INFO) << "Found user " << account.display_name;
              if (account.display_name->size() >= settings_.user.size() &&
                  account.display_name->substr(settings_.user.size()) ==
                      settings_.user) {
                account_id = account.id;
                break;
              }
            }
            if (account_id.empty()) {
              user_provider_->AddUser(
                  modular_auth::IdentityProvider::DEV,
                  [this](modular_auth::AccountPtr account,
                         fidl::StringPtr status) { Login(account->id); });
            } else {
              Login(account_id);
            }
          });
    }
  }

  const Settings settings_;
  fidl::Binding<fuchsia::modular::UserWatcher> user_watcher_binding_;
  fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request_;
  fuchsia::modular::DeviceShellContextPtr device_shell_context_;
  fuchsia::modular::UserControllerPtr user_controller_;
  fuchsia::modular::UserProviderPtr user_provider_;
  fxl::WeakPtrFactory<DevDeviceShellApp> weak_ptr_factory_;
  FXL_DISALLOW_COPY_AND_ASSIGN(DevDeviceShellApp);
};

}  // namespace

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  Settings settings(command_line);

  fsl::MessageLoop loop;

  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  fuchsia::modular::AppDriver<DevDeviceShellApp> driver(
      app_context->outgoing().deprecated_services(),
      std::make_unique<DevDeviceShellApp>(app_context.get(), settings),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
