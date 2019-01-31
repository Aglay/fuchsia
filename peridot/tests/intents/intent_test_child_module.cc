// Copyright 2018 The Fuchsia Authors. All rights reserved.
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
#include "peridot/tests/intents/defs.h"

using modular::testing::Signal;
using modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestModule : fuchsia::modular::IntentHandler {
 public:
  TestModule(modular::ModuleHost* module_host,
             fidl::InterfaceRequest<
                 fuchsia::ui::app::ViewProvider> /*view_provider_request*/) {
    modular::testing::Init(module_host->startup_context(), __FILE__);
    module_host->startup_context()
        ->outgoing()
        .AddPublicService<fuchsia::modular::IntentHandler>(
            [this](fidl::InterfaceRequest<fuchsia::modular::IntentHandler>
                       request) {
              bindings_.AddBinding(this, std::move(request));
            });
  }

  TestModule(modular::ModuleHost* const module_host,
             fidl::InterfaceRequest<
                 fuchsia::ui::viewsv1::ViewProvider> /*view_provider_request*/)
      : TestModule(
            module_host,
            fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider>(nullptr)) {}

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    modular::testing::Done(done);
  }

 private:
  // |IntentHandler|
  void HandleIntent(fuchsia::modular::Intent intent) override {
    for (const auto& parameter : *intent.parameters) {
      if (parameter.data.is_json()) {
        if (parameter.name == kIntentParameterName ||
            parameter.name == kIntentParameterNameAlternate) {
          std::string parameter_data;
          FXL_CHECK(fsl::StringFromVmo(parameter.data.json(), &parameter_data));
          Signal(kChildModuleHandledIntent + parameter_data);
        }
      }
    }
  }

  fidl::BindingSet<fuchsia::modular::IntentHandler> bindings_;

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
