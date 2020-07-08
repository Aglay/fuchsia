// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/story_runner/module_controller_impl.h"

#include <fuchsia/ui/app/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_ptr.h>
#include <lib/fidl/cpp/interface_request.h>

#include "src/modular/bin/sessionmgr/storage/encode_module_path.h"
#include "src/modular/bin/sessionmgr/story_runner/story_controller_impl.h"
#include "src/modular/lib/common/teardown.h"
#include "src/modular/lib/fidl/clone.h"

namespace modular {

ModuleControllerImpl::ModuleControllerImpl(StoryControllerImpl* const story_controller_impl,
                                           fuchsia::sys::Launcher* const launcher,
                                           fuchsia::modular::session::AppConfig module_config,
                                           const fuchsia::modular::ModuleData* const module_data,
                                           fuchsia::sys::ServiceListPtr service_list,
                                           fuchsia::ui::views::ViewToken view_token)
    : story_controller_impl_(story_controller_impl),
      app_client_(launcher, CloneStruct(module_config),
                  /*data_origin=*/"", std::move(service_list)),
      module_data_(module_data) {
  app_client_.SetAppErrorHandler([this] { OnAppConnectionError(); });

  fuchsia::ui::app::ViewProviderPtr view_provider;
  app_client_.services().ConnectToService(view_provider.NewRequest());
  view_provider->CreateView(std::move(view_token.value), nullptr /* incoming_services */,
                            nullptr /* outgoing_services */);
}

ModuleControllerImpl::~ModuleControllerImpl() {}

void ModuleControllerImpl::Connect(
    fidl::InterfaceRequest<fuchsia::modular::ModuleController> request) {
  module_controller_bindings_.AddBinding(this, std::move(request));
  // Notify of initial state on connection.
  NotifyStateChange();
}

// If the ComponentController connection closes, it means the module cannot be
// started. We indicate this by the ERROR state.
void ModuleControllerImpl::OnAppConnectionError() {
  FX_LOGS(ERROR) << "Module " << EncodeModulePath(module_data_->module_path()) << " (URL "
                 << module_data_->module_url() << ") terminated unexpectedly.";
  SetState(fuchsia::modular::ModuleState::ERROR);
}

void ModuleControllerImpl::SetState(const fuchsia::modular::ModuleState new_state) {
  if (state_ == new_state) {
    return;
  }

  state_ = new_state;
  NotifyStateChange();
}

void ModuleControllerImpl::Teardown(fit::function<void()> done) {
  // At this point, it's no longer an error if the module closes its
  // connection, or the application exits.
  app_client_.SetAppErrorHandler(nullptr);

  // Tear down the module application through the normal procedure with timeout.
  app_client_.Teardown(kBasicTimeout, [this, done = std::move(done)] {
    SetState(fuchsia::modular::ModuleState::STOPPED);
    done();
  });
}

void ModuleControllerImpl::Focus() {
  story_controller_impl_->FocusModule(module_data_->module_path());
}

void ModuleControllerImpl::Defocus() {
  story_controller_impl_->DefocusModule(module_data_->module_path());
}

void ModuleControllerImpl::Stop(StopCallback done) {
  story_controller_impl_->DeleteModule(module_data_->module_path(), std::move(done));
}

void ModuleControllerImpl::NotifyStateChange() {
  for (auto& binding : module_controller_bindings_.bindings()) {
    binding->events().OnStateChange(state_);
  }
}

}  // namespace modular
