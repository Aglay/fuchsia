// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/story_runner/module_controller_impl.h"

#include "lib/fidl/cpp/bindings/interface_handle.h"
#include "lib/fidl/cpp/bindings/interface_ptr.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/tasks/task_runner.h"
#include "lib/fxl/time/time_delta.h"
#include "peridot/bin/story_runner/story_controller_impl.h"
#include "peridot/lib/common/teardown.h"

namespace modular {

constexpr char kAppStoragePath[] = "/data/APP_DATA";

namespace {

// A stopgap solution to map a module's url to a directory name where the
// module's /data is mapped. We need three properties here - (1) two module urls
// that are the same get mapped to the same hash, (2) two modules urls that are
// different don't get the same name (with very high probability) and (3) the
// name is visually inspectable.
std::string HashModuleUrl(const std::string& module_url) {
  std::size_t found = module_url.find_last_of('/');
  auto last_part =
      found == module_url.length() - 1 ? "" : module_url.substr(found + 1);
  return std::to_string(std::hash<std::string>{}(module_url)) + last_part;
}

};  // namespace

ModuleControllerImpl::ModuleControllerImpl(
    StoryControllerImpl* const story_controller_impl,
    app::ApplicationLauncher* const application_launcher,
    AppConfigPtr module_config,
    const ModuleData* const module_data,
    app::ServiceListPtr service_list,
    f1dl::InterfaceHandle<ModuleContext> module_context,
    f1dl::InterfaceRequest<mozart::ViewProvider> view_provider_request,
    f1dl::InterfaceRequest<app::ServiceProvider> incoming_services)
    : story_controller_impl_(story_controller_impl),
      app_client_(
          application_launcher,
          module_config.Clone(),
          std::string(kAppStoragePath) + HashModuleUrl(module_config->url),
          std::move(service_list)),
      module_data_(module_data) {
  app_client_.SetAppErrorHandler([this] { SetState(ModuleState::ERROR); });

  app_client_.services().ConnectToService(module_service_.NewRequest());
  module_service_.set_error_handler([this] { OnConnectionError(); });
  module_service_->Initialize(std::move(module_context),
                              std::move(incoming_services));

  app_client_.services().ConnectToService(std::move(view_provider_request));

  // Push the initial module state to story controller. TODO(mesch): This is
  // only needed for the root module to transition the story state to STARTING
  // and get IsRunning() to true. This could be handled inside
  // StoryControllerImpl too.
  story_controller_impl_->OnModuleStateChange(module_data_->module_path,
                                              state_);
}

ModuleControllerImpl::~ModuleControllerImpl() {}

void ModuleControllerImpl::Connect(
    f1dl::InterfaceRequest<ModuleController> request) {
  module_controller_bindings_.AddBinding(this, std::move(request));
}

EmbedModuleControllerPtr ModuleControllerImpl::NewEmbedModuleController() {
  return embed_module_controller_bindings_.AddBinding(this).Bind();
}

// If the Module instance closes its own connection, we signal this to
// all current and future watchers by an appropriate state transition.
void ModuleControllerImpl::OnConnectionError() {
  if (state_ == ModuleState::STARTING) {
    SetState(ModuleState::UNLINKED);
  } else {
    SetState(ModuleState::ERROR);
  }
}

void ModuleControllerImpl::SetState(const ModuleState new_state) {
  if (state_ == new_state) {
    return;
  }

  state_ = new_state;
  watchers_.ForAllPtrs(
      [this](ModuleWatcher* const watcher) { watcher->OnStateChange(state_); });

  story_controller_impl_->OnModuleStateChange(module_data_->module_path,
                                              state_);
}

void ModuleControllerImpl::Teardown(std::function<void()> done) {
  teardown_.push_back(done);

  if (teardown_.size() != 1) {
    // Not the first request, Stop() in progress.
    return;
  }

  auto cont = [this] {
    module_service_.Unbind();
    SetState(ModuleState::STOPPED);

    // ReleaseModule() must be called before the callbacks, because
    // StoryControllerImpl::Stop() relies on being called back *after* the
    // module controller was disposed.
    story_controller_impl_->ReleaseModule(this);

    for (auto& done : teardown_) {
      done();
    }

    // |this| must be deleted after the callbacks so that the |done()| calls
    // above can be dispatched while the bindings still exist in case they are
    // FIDL method callbacks.
    //
    // The destructor of |this| deletes |app_client_|, which will kill the
    // related application if it's still running.
    delete this;
  };

  // At this point, it's no longer an error if the module closes its
  // connection, or the application exits.
  app_client_.SetAppErrorHandler(nullptr);
  module_service_.set_error_handler(nullptr);

  // If the module was UNLINKED, stop it without a delay. Otherwise
  // call Module.Stop(), but also schedule a timeout in case it
  // doesn't return from Stop().
  if (state_ == ModuleState::UNLINKED) {
    fsl::MessageLoop::GetCurrent()->task_runner()->PostTask(cont);
  } else {
    app_client_.Teardown(kBasicTimeout, cont);
  }
}

void ModuleControllerImpl::Watch(f1dl::InterfaceHandle<ModuleWatcher> watcher) {
  auto ptr = watcher.Bind();
  ptr->OnStateChange(state_);
  watchers_.AddInterfacePtr(std::move(ptr));
}

void ModuleControllerImpl::Focus() {
  story_controller_impl_->FocusModule(module_data_->module_path);
}

void ModuleControllerImpl::Defocus() {
  story_controller_impl_->DefocusModule(module_data_->module_path);
}

void ModuleControllerImpl::Stop(const StopCallback& done) {
  story_controller_impl_->StopModule(module_data_->module_path, done);
}

}  // namespace modular
