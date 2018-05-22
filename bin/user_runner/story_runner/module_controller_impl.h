// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_MODULE_CONTROLLER_IMPL_H_
#define PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_MODULE_CONTROLLER_IMPL_H_

#include <vector>

#include <component/cpp/fidl.h>
#include <modular/cpp/fidl.h>
#include <views_v1/cpp/fidl.h>
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fidl/cpp/interface_handle.h"
#include "lib/fidl/cpp/interface_ptr.h"
#include "lib/fidl/cpp/interface_ptr_set.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/fidl/app_client.h"

namespace modular {

class StoryControllerImpl;

// Implements the ModuleController interface, which is given to the
// client that called ModuleContext.(Start|Embed)Module(). Exactly one
// ModuleControllerImpl instance is associated with each
// ModuleContextImpl instance.
class ModuleControllerImpl : ModuleController {
 public:
  ModuleControllerImpl(
      StoryControllerImpl* story_controller_impl,
      component::ApplicationLauncher* application_launcher,
      AppConfig module_config,
      const ModuleData* module_data,
      component::ServiceListPtr service_list,
      fidl::InterfaceRequest<views_v1::ViewProvider> view_provider_request);

  ~ModuleControllerImpl() override;

  void Connect(fidl::InterfaceRequest<ModuleController> request);

  // Notifies all watchers of a state change of the module. Also
  // remembers the state to initialize future added watchers.
  void SetState(ModuleState new_state);

  // Calls Stop() on the module, closes the module handle, notifies
  // watchers, then DisposeModule()s the connection and finally calls
  // done(). Thus, done must not reference anything in
  // ModuleController or the related ModuleContextImpl.
  void Teardown(std::function<void()> done);

 private:
  // |ModuleController|
  void Watch(fidl::InterfaceHandle<ModuleWatcher> watcher) override;

  // |ModuleController|
  void Focus() override;

  // |ModuleController|
  void Defocus() override;

  // |ModuleController|
  void Stop(StopCallback done) override;

  // Used as application error handler on the Module app client.
  void OnAppConnectionError();

  // Used as connection error handler on the Module connection.
  void OnModuleConnectionError();

  // The story this Module instance runs in.
  StoryControllerImpl* const story_controller_impl_;

  AppClient<Lifecycle> app_client_;

  // The Module path and other information about the module instance.
  const ModuleData* const module_data_;

  // The service provided here.
  fidl::BindingSet<ModuleController> module_controller_bindings_;

  // Watchers of this Module instance.
  fidl::InterfacePtrSet<ModuleWatcher> watchers_;

  // The state of this Module instance, stored here to initialize watchers
  // registered in the future to the current state.
  ModuleState state_{ModuleState::RUNNING};

  // Callbacks of Teardown() invocations. If there is one Stop() request
  // pending, a second one is only queued, no second call to Stop() is made.
  std::vector<std::function<void()>> teardown_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleControllerImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_MODULE_CONTROLLER_IMPL_H_
