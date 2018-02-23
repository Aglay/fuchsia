// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/device_runner/user_controller_impl.h"

#include <memory>
#include <utility>

#include "lib/fidl/cpp/bindings/synchronous_interface_ptr.h"
#include "lib/user/fidl/user_runner.fidl-sync.h"
#include "peridot/lib/common/async_holder.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/array_to_string.h"

namespace modular {

UserControllerImpl::UserControllerImpl(
    app::ApplicationLauncher* const application_launcher,
    AppConfigPtr user_runner,
    AppConfigPtr user_shell,
    AppConfigPtr story_shell,
    f1dl::InterfaceHandle<auth::TokenProviderFactory> token_provider_factory,
    auth::AccountPtr account,
    f1dl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
    f1dl::InterfaceHandle<app::ServiceProvider> device_shell_services,
    f1dl::InterfaceRequest<UserController> user_controller_request,
    DoneCallback done)
    : user_context_binding_(this),
      user_controller_binding_(this, std::move(user_controller_request)),
      device_shell_services_(
          device_shell_services ? device_shell_services.Bind() : nullptr),
      done_(std::move(done)) {
  // 0. Generate the path to map '/data' for the user runner we are starting.
  std::string data_origin;
  if (account.is_null()) {
    // Guest user.
    // Generate a random number to be used in this case.
    uint32_t random_number;
    size_t random_size;
    zx_status_t status =
        zx_cprng_draw(&random_number, sizeof random_number, &random_size);
    FXL_CHECK(status == ZX_OK);
    FXL_CHECK(sizeof random_number == random_size);
    data_origin =
        std::string("/tmp/modular/GUEST_USER_") + std::to_string(random_number);
  } else {
    // Non-guest user.
    data_origin = std::string("/data/modular/USER_") + std::string(account->id);
  }

  // 1. Launch UserRunner in the current environment.
  user_runner_app_ = std::make_unique<AppClient<Lifecycle>>(
      application_launcher, std::move(user_runner), data_origin);

  // 2. Initialize the UserRunner service.
  user_runner_app_->services().ConnectToService(user_runner_.NewRequest());
  user_runner_->Initialize(
      std::move(account), std::move(user_shell), std::move(story_shell),
      std::move(token_provider_factory), user_context_binding_.NewBinding(),
      std::move(view_owner_request));
}

std::string UserControllerImpl::DumpState() {
  UserRunnerDebugSyncPtr debug;
  user_runner_app_->services().ConnectToService(
      f1dl::GetSynchronousProxy(&debug));
  f1dl::String output;
  debug->DumpState(&output);
  return output;
}

// |UserController|
void UserControllerImpl::Logout(const LogoutCallback& done) {
  FXL_LOG(INFO) << "UserController::Logout()";
  logout_response_callbacks_.push_back(done);
  if (logout_response_callbacks_.size() > 1) {
    return;
  }

  // This should prevent us from receiving any further requests.
  user_controller_binding_.Unbind();
  user_context_binding_.Unbind();

  user_runner_app_->Teardown(kUserRunnerTimeout, [this] {
    for (const auto& done : logout_response_callbacks_) {
      done();
    }
    // We announce |OnLogout| only at point just before deleting ourselves,
    // so we can avoid any race conditions that may be triggered by |Shutdown|
    // (which in-turn will call this |Logout| since we have not completed yet).
    user_watchers_.ForAllPtrs(
        [](UserWatcher* watcher) { watcher->OnLogout(); });
    done_(this);
  });
}

// |UserContext|
void UserControllerImpl::GetPresentation(
    f1dl::InterfaceRequest<mozart::Presentation> presentation) {
  if (device_shell_services_) {
    device_shell_services_->ConnectToService("mozart.Presentation",
                                             presentation.TakeChannel());
  }
}

// |UserController|
void UserControllerImpl::Watch(f1dl::InterfaceHandle<UserWatcher> watcher) {
  user_watchers_.AddInterfacePtr(watcher.Bind());
}

// |UserContext|
// TODO(alhaad): Reconcile UserContext.Logout() and UserControllerImpl.Logout().
void UserControllerImpl::Logout() {
  FXL_LOG(INFO) << "UserContext::Logout()";
  Logout([] {});
}

}  // namespace modular
