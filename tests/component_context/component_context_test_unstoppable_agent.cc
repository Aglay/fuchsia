// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include "lib/app_driver/cpp/agent_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/logging.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

using modular::testing::TestPoint;

namespace {

class UnstoppableAgentApp {
 public:
  TestPoint initialized_{"Unstoppable agent initialized"};

  UnstoppableAgentApp(modular::AgentHost* agent_host) {
    modular::testing::Init(agent_host->application_context(), __FILE__);
    agent_host->agent_context()->GetComponentContext(
        component_context_.NewRequest());
    initialized_.Pass();
  }

  // Called by AgentDriver.
  void Connect(
      f1dl::InterfaceRequest<component::ServiceProvider> /*services*/) {}

  // Called by AgentDriver.
  void RunTask(const f1dl::StringPtr& /*task_id*/,
               const std::function<void()>& /*callback*/) {}

  TestPoint stopped_{"Unstoppable agent stopped"};

  // Called by AgentDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  modular::AgentContextPtr agent_context_;
  modular::ComponentContextPtr component_context_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::AgentDriver<UnstoppableAgentApp> driver(app_context.get(),
                                                   [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
