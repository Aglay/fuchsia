// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/views_v1/cpp/fidl.h>
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/embed_shell/defs.h"

using fuchsia::modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestApp : fuchsia::modular::ModuleWatcher {
 public:
  TestApp(fuchsia::modular::ModuleHost* const module_host,
          fidl::InterfaceRequest<
              fuchsia::ui::views_v1::ViewProvider> /*view_provider_request*/)
      : module_host_(module_host) {
    fuchsia::modular::testing::Init(module_host->startup_context(), __FILE__);
    StartChildModule();
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    fuchsia::modular::testing::Done(done);
  }

 private:
  void StartChildModule() {
    fuchsia::modular::Intent intent;
    intent.action.handler = kCommonNullModule;
    module_host_->module_context()->StartModule(
        kChildModuleName, std::move(intent), child_module_.NewRequest(),
        nullptr /* surface_relation */,
        [](const fuchsia::modular::StartModuleStatus) {});

    child_module_->Watch(module_watcher_.AddBinding(this));
  }

  // |ModuleWatcher|
  void OnStateChange(fuchsia::modular::ModuleState state) override {
    if (state == fuchsia::modular::ModuleState::RUNNING) {
      fuchsia::modular::testing::GetStore()->Put("child_module_done", "1",
                                                 [] {});
    }
  }

  fuchsia::modular::ModuleHost* const module_host_;
  fuchsia::modular::ModuleControllerPtr child_module_;
  fidl::BindingSet<fuchsia::modular::ModuleWatcher> module_watcher_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto context = fuchsia::sys::StartupContext::CreateFromStartupInfo();
  fuchsia::modular::ModuleDriver<TestApp> driver(context.get(),
                                                 [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
