// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_DEVICE_RUNNER_USER_CONTROLLER_IMPL_H_
#define PERIDOT_BIN_DEVICE_RUNNER_USER_CONTROLLER_IMPL_H_

#include <fuchsia/cpp/component.h>
#include <fuchsia/cpp/modular_auth.h>
#include <fuchsia/cpp/presentation.h>
#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/application_context.h"
#include "lib/auth/fidl/account/account.fidl.h"
#include "lib/config/fidl/config.fidl.h"
#include "lib/device/fidl/user_provider.fidl.h"
#include "lib/fidl/cpp/array.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fidl/cpp/bindings/interface_handle.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fxl/macros.h"
#include "lib/lifecycle/fidl/lifecycle.fidl.h"
#include "lib/user/fidl/user_runner.fidl.h"
#include "peridot/lib/fidl/app_client.h"
#include "peridot/lib/fidl/scope.h"

namespace modular {

// |UserControllerImpl| starts and manages a UserRunner. The life time of a
// UserRunner is bound to this class.  |UserControllerImpl| is not self-owned,
// but still drives its own deletion: On logout, it signals its
// owner (DeviceRunnerApp) to delete it.
class UserControllerImpl : UserController, UserContext {
 public:
  // After perfoming logout, to signal our completion (and deletion of our
  // instance) to our owner, we do it using a callback supplied to us in our
  // constructor. (The alternative is to take in a DeviceRunnerApp*, which seems
  // a little specific and overscoped).
  using DoneCallback = std::function<void(UserControllerImpl*)>;

  UserControllerImpl(
      component::ApplicationLauncher* application_launcher,
      AppConfigPtr user_runner,
      AppConfigPtr user_shell,
      AppConfigPtr story_shell,
      f1dl::InterfaceHandle<auth::TokenProviderFactory> token_provider_factory,
      auth::AccountPtr account,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      f1dl::InterfaceHandle<component::ServiceProvider> device_shell_services,
      f1dl::InterfaceRequest<UserController> user_controller_request,
      DoneCallback done);

  std::string DumpState();

  // This will effectively tear down the entire instance by calling |done|.
  // |UserController|
  void Logout(const LogoutCallback& done) override;

 private:
  // |UserController|
  void SwapUserShell(AppConfigPtr user_shell,
                     const SwapUserShellCallback& callback) override;

  // |UserController|
  void Watch(f1dl::InterfaceHandle<UserWatcher> watcher) override;

  // |UserContext|
  void Logout() override;

  // |UserContext|
  void GetPresentation(
      f1dl::InterfaceRequest<presentation::Presentation> presentation) override;

  std::unique_ptr<Scope> user_runner_scope_;
  std::unique_ptr<AppClient<Lifecycle>> user_runner_app_;
  UserRunnerPtr user_runner_;

  f1dl::Binding<UserContext> user_context_binding_;
  f1dl::Binding<UserController> user_controller_binding_;

  f1dl::InterfacePtrSet<modular::UserWatcher> user_watchers_;

  std::vector<LogoutCallback> logout_response_callbacks_;

  component::ServiceProviderPtr device_shell_services_;

  DoneCallback done_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UserControllerImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_DEVICE_RUNNER_USER_CONTROLLER_IMPL_H_
