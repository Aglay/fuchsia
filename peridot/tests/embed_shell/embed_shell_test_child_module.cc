// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <lib/app_driver/cpp/module_driver.h>
#include <lib/async-loop/cpp/loop.h>

#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/embed_shell/defs.h"

using modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestModule {
 public:
  TestModule(modular::ModuleHost* const module_host,
             fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider>
                 view_provider_request)
      : module_host_(module_host),
        app_view_provider_(std::move(view_provider_request)) {
    modular::testing::Init(module_host->startup_context(), __FILE__);
    StartChildModule();
  }

  TestModule(modular::ModuleHost* const module_host,
             fidl::InterfaceRequest<fuchsia::ui::viewsv1::ViewProvider>
                 view_provider_request)
      : TestModule(
            module_host,
            fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider>(nullptr)) {
    views1_view_provider_ = std::move(view_provider_request);
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    modular::testing::Done(done);
  }

 private:
  void StartChildModule() {
    child_module_.events().OnStateChange =
        [this](fuchsia::modular::ModuleState new_state) {
          OnStateChange(std::move(new_state));
        };

    fuchsia::modular::Intent intent;
    intent.handler = kCommonNullModule;
    intent.action = kCommonNullAction;
    module_host_->module_context()->AddModuleToStory(
        kChildModuleName, std::move(intent), child_module_.NewRequest(),
        nullptr /* surface_relation */,
        [](const fuchsia::modular::StartModuleStatus) {});
  }

  void OnStateChange(fuchsia::modular::ModuleState state) {
    if (state == fuchsia::modular::ModuleState::RUNNING) {
      modular::testing::GetStore()->Put("child_module_done", "1", [] {});
    }
  }

  modular::ModuleHost* const module_host_;

  // We keep the view provider around so that story shell can hold a view for
  // us, but don't do anything with it.
  fidl::InterfaceRequest<fuchsia::ui::viewsv1::ViewProvider>
      views1_view_provider_;
  fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> app_view_provider_;

  fuchsia::modular::ModuleControllerPtr child_module_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestModule);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = component::StartupContext::CreateFromStartupInfo();
  modular::ModuleDriver<TestModule> driver(context.get(),
                                           [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
