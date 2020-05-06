// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of the fuchsia::modular::BaseShell service that passes a
// command line configurable user name to its fuchsia::modular::UserProvider,
// and is able to run a story with a single module through its life cycle.

#include <fuchsia/auth/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>
#include <utility>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/modular/lib/app_driver/cpp/app_driver.h"
#include "src/modular/lib/fidl/single_service_app.h"

namespace modular {

class Settings {
 public:
  explicit Settings(const fxl::CommandLine& command_line) {
    // defaults to auto-logging in as an ephemeral user
    persist_user = command_line.HasOption("persist_user");
  }

  bool persist_user;
};

class AutoLoginBaseShellApp : modular::SingleServiceApp<fuchsia::modular::BaseShell> {
 public:
  explicit AutoLoginBaseShellApp(sys::ComponentContext* const component_context, Settings settings)
      : SingleServiceApp(component_context),
        settings_(std::move(settings)),
        weak_ptr_factory_(this) {}

  ~AutoLoginBaseShellApp() override = default;

  // |SingleServiceApp|
  void Terminate(fit::function<void()> done) override { done(); }

 private:
  // |SingleServiceApp|
  void CreateView(
      zx::eventpair view_token,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> /*incoming_services*/,
      fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> /*outgoing_services*/) override {
    view_token_.value = std::move(view_token);

    Connect();
  }

  // |fuchsia::modular::BaseShell|
  void Initialize(fidl::InterfaceHandle<fuchsia::modular::BaseShellContext> base_shell_context,
                  fuchsia::modular::BaseShellParams) override {
    base_shell_context_.Bind(std::move(base_shell_context));
    base_shell_context_->GetUserProvider(user_provider_.NewRequest());

    Connect();
  }

  // |fuchsia::modular::BaseShell|
  void GetAuthenticationUIContext(
      fidl::InterfaceRequest<fuchsia::auth::AuthenticationUIContext> /*request*/) override {
    FX_LOGS(INFO) << "fuchsia::modular::BaseShell::GetAuthenticationUIContext() is"
                     " unimplemented.";
  }

  void Connect() {
    if (user_provider_ && view_token_.value) {
      bool is_ephemeral_account = !settings_.persist_user;
      user_provider_->Login3(is_ephemeral_account);
    }
  }

  const Settings settings_;
  fuchsia::ui::views::ViewToken view_token_;
  fuchsia::modular::BaseShellContextPtr base_shell_context_;
  fuchsia::modular::UserProviderPtr user_provider_;

  fxl::WeakPtrFactory<AutoLoginBaseShellApp> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AutoLoginBaseShellApp);
};

}  // namespace modular

int main(int argc, const char** argv) {
  syslog::SetTags({"auto_login_base_shell"});

  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  modular::Settings settings(command_line);

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  auto context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
  modular::AppDriver<modular::AutoLoginBaseShellApp> driver(
      context->outgoing(),
      std::make_unique<modular::AutoLoginBaseShellApp>(context.get(), settings),
      [&loop] { loop.Quit(); });

  loop.Run();
  return 0;
}
