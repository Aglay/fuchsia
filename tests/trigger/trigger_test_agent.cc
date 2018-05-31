// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <modular_test_trigger/cpp/fidl.h>
#include "lib/app_driver/cpp/agent_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/logging.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/trigger/defs.h"

using fuchsia::modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestApp : modular_test_trigger::TriggerTestService {
 public:
  TestPoint initialized_{"Trigger test agent initialized"};

  TestApp(fuchsia::modular::AgentHost* const agent_host)
      : agent_context_(agent_host->agent_context()) {
    fuchsia::modular::testing::Init(agent_host->application_context(),
                                    __FILE__);
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

    agent_services_.AddService<modular_test_trigger::TriggerTestService>(
        [this](fidl::InterfaceRequest<modular_test_trigger::TriggerTestService>
                   request) {
          service_bindings_.AddBinding(this, std::move(request));
        });

    initialized_.Pass();
  }

  // Called by AgentDriver.
  void Connect(fidl::InterfaceRequest<component::ServiceProvider> services) {
    agent_services_.AddBinding(std::move(services));
    fuchsia::modular::testing::GetStore()->Put("trigger_test_agent_connected",
                                               "", [] {});
  }

  // Called by AgentDriver.
  void RunTask(fidl::StringPtr task_id, const std::function<void()>& callback) {
    fuchsia::modular::testing::GetStore()->Put(task_id, "", callback);
  }

  // Called by AgentDriver.
  void Terminate(const std::function<void()>& done) {
    fuchsia::modular::testing::GetStore()->Put("trigger_test_agent_stopped", "",
                                               [done] { done(); });
    fuchsia::modular::testing::Done(done);
  }

 private:
  // |TriggerTestService|
  void GetMessageQueueToken(GetMessageQueueTokenCallback callback) override {
    msg_queue_->GetToken(
        [callback](fidl::StringPtr token) { callback(token); });
  }

  // |TriggerTestService|
  void ObserveMessageQueueDeletion(fidl::StringPtr queue_token) override {
    fuchsia::modular::TaskInfo task_info;
    task_info.task_id = queue_token;
    task_info.trigger_condition.set_queue_deleted(queue_token);
    task_info.persistent = true;
    agent_context_->ScheduleTask(std::move(task_info));
    fuchsia::modular::testing::GetStore()->Put(
        "trigger_test_agent_token_received", "", [] {});
  }

  component::ServiceNamespace agent_services_;

  fuchsia::modular::ComponentContextPtr component_context_;
  fuchsia::modular::AgentContext* const agent_context_;
  fuchsia::modular::MessageQueuePtr msg_queue_;

  fidl::BindingSet<modular_test_trigger::TriggerTestService> service_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  fuchsia::modular::AgentDriver<TestApp> driver(app_context.get(),
                                                [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
