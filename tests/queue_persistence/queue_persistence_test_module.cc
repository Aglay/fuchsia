// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/queue_persistence_test_service.h>
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
#include "peridot/tests/queue_persistence/defs.h"

namespace {

// Cf. README.md for what this test does and how.
class ParentApp {
 public:
  ParentApp(
      modular::ModuleHost* module_host,
      fidl::InterfaceRequest<views_v1::ViewProvider> /*view_provider_request*/,
      fidl::InterfaceRequest<component::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host), weak_ptr_factory_(this) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    initialized_.Pass();

    module_host_->module_context()->GetComponentContext(
        component_context_.NewRequest());

    component::ServiceProviderPtr agent_services;
    component_context_->ConnectToAgent(kTestAgent, agent_services.NewRequest(),
                                       agent_controller_.NewRequest());
    ConnectToService(agent_services.get(), agent_service_.NewRequest());

    modular::testing::GetStore()->Get(
        "queue_persistence_test_agent_connected",
        [this](const fidl::StringPtr&) { AgentConnected(); });

    // Start a timer to call Story.Done() in case the test agent misbehaves and
    // we time out. If that happens, the module will exit normally through
    // Stop(), but the test will fail because some TestPoints will not have been
    // passed.
    async::PostDelayedTask(
        async_get_default(),
        callback::MakeScoped(
            weak_ptr_factory_.GetWeakPtr(),
            [this] { module_host_->module_context()->Done(); }),
        zx::msec(kTimeoutMilliseconds));
  }

  // Called by ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  void AgentConnected() {
    agent_connected_.Pass();
    agent_service_->GetMessageQueueToken(
        [this](const fidl::StringPtr& token) { ReceivedQueueToken(token); });
  }

  void ReceivedQueueToken(const fidl::StringPtr& token) {
    queue_token_ = token;
    received_queue_persistence_token_.Pass();

    // Stop the agent.
    agent_controller_.Unbind();
    agent_service_.Unbind();
    modular::testing::GetStore()->Get(
        "queue_persistence_test_agent_stopped",
        [this](const fidl::StringPtr&) { AgentStopped(); });
  }

  void AgentStopped() {
    agent_stopped_.Pass();

    // Send a message to the stopped agent which should be persisted to local
    // storage. No triggers are set so the agent won't be automatically started.
    modular::MessageSenderPtr message_sender;
    component_context_->GetMessageSender(queue_token_,
                                         message_sender.NewRequest());
    message_sender->Send("Queued message...");

    // Start the agent again.
    component::ServiceProviderPtr agent_services;
    component_context_->ConnectToAgent(kTestAgent, agent_services.NewRequest(),
                                       agent_controller_.NewRequest());
    ConnectToService(agent_services.get(), agent_service_.NewRequest());

    modular::testing::GetStore()->Get(
        "queue_persistence_test_agent_connected",
        [this](const fidl::StringPtr&) { AgentConnectedAgain(); });
  }

  void AgentConnectedAgain() {
    agent_connected_again_.Pass();
    modular::testing::GetStore()->Get(
        "queue_persistence_test_agent_received_message",
        [this](const fidl::StringPtr&) { AgentReceivedMessage(); });
  }

  void AgentReceivedMessage() {
    agent_received_message_.Pass();

    // Stop the agent again.
    agent_controller_.Unbind();
    agent_service_.Unbind();
    modular::testing::GetStore()->Get("queue_persistence_test_agent_stopped",
                                      [this](const fidl::StringPtr&) {
                                        module_host_->module_context()->Done();
                                      });
  }

  modular::ModuleHost* module_host_;
  modular::AgentControllerPtr agent_controller_;
  queue_persistence_test_service::QueuePersistenceTestServicePtr agent_service_;
  modular::ComponentContextPtr component_context_;
  modular::MessageQueuePtr msg_queue_;

  std::string queue_token_;

  using TestPoint = modular::testing::TestPoint;
  TestPoint initialized_{"Root module initialized"};
  TestPoint received_queue_persistence_token_{
      "Received queue_persistence token"};
  TestPoint stopped_{"Root module stopped"};
  TestPoint agent_connected_{"Agent accepted connection"};
  TestPoint agent_connected_again_{"Agent accepted connection, again"};
  TestPoint agent_received_message_{"Agent received message"};
  TestPoint agent_stopped_{"Agent stopped"};

  fxl::WeakPtrFactory<ParentApp> weak_ptr_factory_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<ParentApp> driver(app_context.get(),
                                          [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
