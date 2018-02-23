// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/ui/views/fidl/view_token.fidl.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

using modular::testing::TestPoint;

namespace {

// The NullModule just sits there and does nothing until it's terminated.
class NullModule {
 public:
  NullModule(
      modular::ModuleHost* const module_host,
      f1dl::InterfaceRequest<mozart::ViewProvider> /*view_provider_request*/,
      f1dl::InterfaceRequest<app::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host) {
    modular::testing::Init(module_host_->application_context(), __FILE__);
    module_host_->module_context()->Ready();
    initialized_.Pass();
  }

  // Called by ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  TestPoint initialized_{"Null module initialized"};
  TestPoint stopped_{"Null module stopped"};

  modular::ModuleHost* const module_host_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = app::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<NullModule> driver(app_context.get(),
                                           [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
