// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1.h>
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/time/time_delta.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

using modular::testing::TestPoint;

namespace {

constexpr char kLink[] = "context_link";

// Cf. README.md for what this test does and how.
class TestApp {
 public:
  TestApp(
      modular::ModuleHost* module_host,
      fidl::InterfaceRequest<views_v1::ViewProvider> /*view_provider_request*/,
      fidl::InterfaceRequest<
          component::ServiceProvider> /*outgoing_services*/) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    initialized_.Pass();
    module_host->module_context()->GetLink(kLink, link_.NewRequest());
    Set1();
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  void Set1() {
    link_->Set(nullptr,
               "{\"link_value\":\"1\",\"@context\":{\"topic\":\""
               "context_link_test\"}}");
    link_->Sync([this] { Set2(); });
  }

  void Set2() {
    link_->Set(nullptr,
               "{\"link_value\":\"2\",\"@context\":{\"topic\":\""
               "context_link_test\"}}");
  }

  TestPoint initialized_{"Child module initialized"};
  TestPoint stopped_{"Child module stopped"};
  modular::LinkPtr link_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<TestApp> driver(app_context.get(),
                                        [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
