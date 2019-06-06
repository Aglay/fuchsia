// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/app_driver/cpp/agent_driver.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/svc/cpp/service_namespace.h>
#include <src/lib/fxl/logging.h>
#include <test/peridot/tests/trigger/cpp/fidl.h>

#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/trigger/defs.h"

using ::modular::testing::TestPoint;
using ::test::peridot::tests::trigger::TriggerTestService;

namespace {

// Cf. README.md for what this test does and how.
class TestApp : TriggerTestService {
 public:
  TestPoint initialized_{"Trigger test agent initialized"};

  TestApp(modular::AgentHost* const agent_host)
      : agent_context_(agent_host->agent_context()) {
    modular::testing::Init(agent_host->component_context(), __FILE__);
    agent_context_->GetComponentContext(component_context_.NewRequest());

    // Create a message queue and schedule a task to be run on receiving a
    // message on it. This message queue is passed to the module.
    component_context_->ObtainMessageQueue("Trigger Queue",
                                           msg_queue_.NewRequest());
    fuchsia::modular::TaskInfo task_info;
    task_info.task_id = "message_queue_message";
    task_info.trigger_condition.set_message_on_queue("Trigger Queue");
    task_info.persistent = true;
    agent_host->agent_context()->ScheduleTask(std::move(task_info));

    agent_services_.AddService<TriggerTestService>(
        [this](fidl::InterfaceRequest<TriggerTestService> request) {
          service_bindings_.AddBinding(this, std::move(request));
        });

    initialized_.Pass();
  }

  // Called by AgentDriver.
  void Connect(fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> services) {
    agent_services_.AddBinding(std::move(services));
    modular::testing::GetStore()->Put("trigger_test_agent_connected", "",
                                      [] {});
  }

  // Called by AgentDriver.
  void RunTask(fidl::StringPtr task_id, fit::function<void()> callback) {
    modular::testing::GetStore()->Put(task_id, "", std::move(callback));
  }

  // Called by AgentDriver.
  void Terminate(fit::function<void()> done) {
    modular::testing::GetStore()->Put("trigger_test_agent_stopped", "",
                                      [done = std::move(done)]() mutable {
                                        modular::testing::Done(std::move(done));
                                      });
  }

 private:
  // |TriggerTestService|
  void GetMessageQueueToken(GetMessageQueueTokenCallback callback) override {
    msg_queue_->GetToken([callback = std::move(callback)](
                             fidl::StringPtr token) { callback(token); });
  }

  // |TriggerTestService|
  void ObserveMessageQueueDeletion(std::string queue_token) override {
    fuchsia::modular::TaskInfo task_info;
    task_info.task_id = queue_token;
    task_info.trigger_condition.set_queue_deleted(queue_token);
    task_info.persistent = true;
    agent_context_->ScheduleTask(std::move(task_info));
    modular::testing::GetStore()->Put("trigger_test_agent_token_received", "",
                                      [] {});
  }

  component::ServiceNamespace agent_services_;

  fuchsia::modular::ComponentContextPtr component_context_;
  fuchsia::modular::AgentContext* const agent_context_;
  fuchsia::modular::MessageQueuePtr msg_queue_;

  fidl::BindingSet<TriggerTestService> service_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = sys::ComponentContext::Create();
  modular::AgentDriver<TestApp> driver(context.get(), [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
