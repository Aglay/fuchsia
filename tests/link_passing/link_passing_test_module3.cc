// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <lib/app_driver/cpp/module_driver.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fsl/vmo/strings.h>

#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/link_passing/defs.h"

using modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestModule : fuchsia::modular::LinkWatcher {
 public:
  TestModule(modular::ModuleHost* const module_host,
             fidl::InterfaceRequest<
                 fuchsia::ui::app::ViewProvider> /*view_provider_request*/)
      : module_host_(module_host),
        link1_watcher_binding_(this),
        link2_watcher_binding_(this) {
    modular::testing::Init(module_host->startup_context(), __FILE__);
    modular::testing::GetStore()->Put("module3_init", "", [] {});

    Start();
  }

  TestModule(modular::ModuleHost* const module_host,
             fidl::InterfaceRequest<
                 fuchsia::ui::viewsv1::ViewProvider> /*view_provider_request*/)
      : TestModule(
            module_host,
            fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider>(nullptr)) {}

  void Start() {
    module_host_->module_context()->GetLink("link1", link1_.NewRequest());
    link1_->WatchAll(link1_watcher_binding_.NewBinding());

    module_host_->module_context()->GetLink("link2", link2_.NewRequest());
    link2_->WatchAll(link2_watcher_binding_.NewBinding());

    fsl::SizedVmo vmo1;
    FXL_CHECK(fsl::VmoFromString("1", &vmo1));
    fsl::SizedVmo vmo2;
    FXL_CHECK(fsl::VmoFromString("2", &vmo2));
    link1_->Set(nullptr, std::move(vmo1).ToTransport());
    link2_->Set(nullptr, std::move(vmo2).ToTransport());
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    modular::testing::GetStore()->Put("module3_stop", "", [] {});
    modular::testing::Done(done);
  }

 private:
  // |fuchsia::modular::LinkWatcher|
  void Notify(fuchsia::mem::Buffer content) override {
    std::string json;
    FXL_CHECK(fsl::StringFromVmo(content, &json));
    FXL_LOG(INFO) << "module3 link: " << json;
  }

  modular::ModuleHost* const module_host_;
  fuchsia::modular::LinkPtr link1_;
  fidl::Binding<fuchsia::modular::LinkWatcher> link1_watcher_binding_;
  fuchsia::modular::LinkPtr link2_;
  fidl::Binding<fuchsia::modular::LinkWatcher> link2_watcher_binding_;

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
