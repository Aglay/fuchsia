// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/modular_test_trigger.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "lib/app/cpp/connect.h"
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/callback/scoped_callback.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/trigger/defs.h"

using modular::testing::TestPoint;

namespace {

// Waits for |condition| to be available in the TestRunnerStore before calling
// |cont| with the result.
void Await(fidl::StringPtr condition,
           std::function<void(fidl::StringPtr)> cont) {
  modular::testing::GetStore()->Get(condition,
                                    [cont](fidl::StringPtr str) { cont(str); });
}

// Cf. README.md for what this test does and how.
class TestApp {
 public:
  TestPoint initialized_{"Root module initialized"};

  TestApp(
      modular::ModuleHost* const module_host,
      fidl::InterfaceRequest<views_v1::ViewProvider> /*view_provider_request*/,
      fidl::InterfaceRequest<component::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host), weak_ptr_factory_(this) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    initialized_.Pass();

    // Exercise ComponentContext.ConnectToAgent()
    module_host_->module_context()->GetComponentContext(
        component_context_.NewRequest());

    component::ServiceProviderPtr agent_services;
    component_context_->ConnectToAgent(kTestAgent, agent_services.NewRequest(),
                                       agent_controller_.NewRequest());
    ConnectToService(agent_services.get(), agent_service_.NewRequest());

    // The message queue that is used to verify deletion triggers.
    component_context_->ObtainMessageQueue("test", msg_queue_.NewRequest());
    msg_queue_->GetToken([this](fidl::StringPtr token) {
      agent_service_->ObserveMessageQueueDeletion(token);
      TestMessageQueueMessageTrigger();
    });

    // Start a timer to quit in case another test component misbehaves and we
    // time out.
    async::PostDelayedTask(
        async_get_default(),
        callback::MakeScoped(
            weak_ptr_factory_.GetWeakPtr(),
            [this] { module_host_->module_context()->Done(); }),
        zx::msec(kTimeoutMilliseconds));
  }

  TestPoint received_trigger_token_{"Received trigger token"};
  TestPoint agent_connected_{"Agent accepted connection"};
  TestPoint agent_stopped_{"Agent1 stopped"};
  TestPoint task_triggered_{"Agent task triggered"};
  void TestMessageQueueMessageTrigger() {
    Await("trigger_test_agent_connected", [this](fidl::StringPtr) {
      agent_connected_.Pass();
      agent_service_->GetMessageQueueToken([this](fidl::StringPtr token) {
        received_trigger_token_.Pass();

        // Stop the agent.
        agent_controller_.Unbind();

        Await("trigger_test_agent_stopped", [this, token](fidl::StringPtr) {
          agent_stopped_.Pass();

          // Send a message to the stopped agent which should
          // trigger it.
          modular::MessageSenderPtr message_sender;
          component_context_->GetMessageSender(token,
                                               message_sender.NewRequest());
          message_sender->Send("Time to wake up...");

          Await("task_id", [this](fidl::StringPtr) {
            task_triggered_.Pass();
            Await("trigger_test_agent_stopped", [this](fidl::StringPtr) {
              TestMessageQueueDeletionTrigger();
            });
          });
        });
      });
    });
  }

  TestPoint queue_deleted_{"Message queue deletion task triggered."};
  void TestMessageQueueDeletionTrigger() {
    component::ServiceProviderPtr agent_services;
    component_context_->ConnectToAgent(kTestAgent, agent_services.NewRequest(),
                                       agent_controller_.NewRequest());
    ConnectToService(agent_services.get(), agent_service_.NewRequest());

    // First wait for the agent to connect, and then kill it.
    Await("trigger_test_agent_connected", [this](fidl::StringPtr) {
      Await("trigger_test_agent_token_received", [this](fidl::StringPtr) {
        agent_controller_.Unbind();
        Await("trigger_test_agent_stopped", [this](fidl::StringPtr) {
          // When the agent has stopped, delete the message queue and verify
          // that the agent is woken up and notified.
          component_context_->DeleteMessageQueue("test");
          Await("message_queue_deletion", [this](fidl::StringPtr) {
            queue_deleted_.Pass();
            module_host_->module_context()->Done();
          });
        });
      });
    });
  }

  TestPoint stopped_{"Root module stopped"};

  // Called by ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  modular::ModuleHost* const module_host_;
  modular::AgentControllerPtr agent_controller_;
  modular_test_trigger::TriggerTestServicePtr agent_service_;
  modular::ComponentContextPtr component_context_;
  modular::MessageQueuePtr msg_queue_;
  fxl::WeakPtrFactory<TestApp> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
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
