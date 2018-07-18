// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/views_v1/cpp/fidl.h>
#include <lib/app_driver/cpp/module_driver.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fxl/time/time_delta.h>

#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/link_context_entities/defs.h"

using modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestApp {
 public:
  TestPoint initialized_{"Child module initialized"};

  TestApp(modular::ModuleHost* module_host,
          fidl::InterfaceRequest<
              fuchsia::ui::views_v1::ViewProvider> /*view_provider_request*/) {
    modular::testing::Init(module_host->startup_context(), __FILE__);
    initialized_.Pass();
    module_host->module_context()->GetLink("link1", link1_.NewRequest());
    module_host->module_context()->GetLink("link2", link2_.NewRequest());
    Set1();
  }

  TestPoint stopped_{"Child module stopped"};

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  void Set1() {
    link1_->Set(nullptr, R"({"@type": "type1", "value": "value1"})");
    link2_->Set(nullptr,
                R"({"a_property": {"@type": "type2", "value": "value2"}})");
    // TODO(thatguy): When we have fuchsia::modular::Entity support in
    // fuchsia::modular::ContextWriter, create a simple fuchsia::modular::Entity
    // reference and slap it into the fuchsia::modular::Link.
  }

  fuchsia::modular::LinkPtr link1_;
  fuchsia::modular::LinkPtr link2_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = component::StartupContext::CreateFromStartupInfo();
  modular::ModuleDriver<TestApp> driver(context.get(),
                                        [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
