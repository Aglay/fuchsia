// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_MODULE_CONTROLLER_IMPL_H_
#define PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_MODULE_CONTROLLER_IMPL_H_

#include <vector>

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_ptr.h>
#include <lib/fidl/cpp/interface_ptr_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fxl/macros.h>

#include "peridot/lib/fidl/app_client.h"

namespace modular {

class StoryControllerImpl;

// Implements the fuchsia::modular::ModuleController interface, which is given
// to the client that called
// fuchsia::modular::ModuleContext.(Start|Embed)Module(). Exactly one
// ModuleControllerImpl instance is associated with each
// ModuleContextImpl instance.
class ModuleControllerImpl : fuchsia::modular::ModuleController {
 public:
  ModuleControllerImpl(
      StoryControllerImpl* story_controller_impl,
      fuchsia::sys::Launcher* launcher,
      fuchsia::modular::AppConfig module_config,
      const fuchsia::modular::ModuleData* module_data,
      fuchsia::sys::ServiceListPtr service_list,
      fidl::InterfaceRequest<fuchsia::ui::viewsv1::ViewProvider>
          view_provider_request);

  ~ModuleControllerImpl() override;

  void Connect(
      fidl::InterfaceRequest<fuchsia::modular::ModuleController> request);

  // Notifies of a state change of the module.
  void SetState(fuchsia::modular::ModuleState new_state);

  // Calls Teardown() on the AppClient of the module component instance,
  // notifies state change, then ReleaseModule()s the connection and finally
  // calls |done|.
  //
  // Multiple calls to Teardown() are allowed, and all |done| callbacks are run
  // in order when teardown is complete.
  void Teardown(std::function<void()> done);

  component::Services& services() {
    return app_client_.services();
  }

 private:
  // |fuchsia::modular::ModuleController|
  void Focus() override;

  // |fuchsia::modular::ModuleController|
  void Defocus() override;

  // |fuchsia::modular::ModuleController|
  void Stop(StopCallback done) override;

  // Call to dispatch |ModuleController|'s OnStateChange event.
  void NotifyStateChange();

  // Used as application error handler on the Module app client.
  void OnAppConnectionError();

  // The story this Module instance runs in.
  StoryControllerImpl* const story_controller_impl_;

  AppClient<fuchsia::modular::Lifecycle> app_client_;

  // The Module path and other information about the module instance.
  const fuchsia::modular::ModuleData* const module_data_;

  // The service provided here.
  fidl::BindingSet<fuchsia::modular::ModuleController>
      module_controller_bindings_;

  // The state of this Module instance. Stored here to notify through events
  // when it changes.
  fuchsia::modular::ModuleState state_{fuchsia::modular::ModuleState::RUNNING};

  // Callbacks passed to Teardown() calls. If there is one Stop() request
  // pending, a second one is only queued, no second call to Stop() is made.
  std::vector<std::function<void()>> teardown_done_callbacks_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleControllerImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_MODULE_CONTROLLER_IMPL_H_
