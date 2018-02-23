// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/app/cpp/connect.h"
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/component/fidl/component_context.fidl.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "lib/module/fidl/module.fidl.h"
#include "peridot/lib/callback/scoped_callback.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/trigger/trigger_test_service.fidl.h"

using modular::testing::TestPoint;

namespace {

// This is how long we wait for the test to finish before we timeout and tear
// down our test.
constexpr int kTimeoutMilliseconds = 10000;
constexpr char kTestAgent[] =
    "file:///system/test/modular_tests/trigger_test_agent";

class ParentApp {
 public:
  TestPoint initialized_{"Root module initialized"};
  TestPoint received_trigger_token_{"Received trigger token"};
  TestPoint agent_connected_{"Agent accepted connection"};
  TestPoint agent_stopped_{"Agent1 stopped"};
  TestPoint task_triggered_{"Agent task triggered"};

  ParentApp(
      modular::ModuleHost* const module_host,
      f1dl::InterfaceRequest<mozart::ViewProvider> /*view_provider_request*/,
      f1dl::InterfaceRequest<app::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host), weak_ptr_factory_(this) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    initialized_.Pass();

    // Exercise ComponentContext.ConnectToAgent()
    module_host_->module_context()->GetComponentContext(
        component_context_.NewRequest());

    app::ServiceProviderPtr agent_services;
    component_context_->ConnectToAgent(kTestAgent, agent_services.NewRequest(),
                                       agent_controller_.NewRequest());
    ConnectToService(agent_services.get(), agent_service_.NewRequest());

    modular::testing::GetStore()->Get(
        "trigger_test_agent_connected", [this](const f1dl::String&) {
          agent_connected_.Pass();
          agent_service_->GetMessageQueueToken(
              [this](const f1dl::String& token) {
                received_trigger_token_.Pass();

                // Stop the agent.
                agent_controller_.Unbind();
                modular::testing::GetStore()->Get(
                    "trigger_test_agent_stopped",
                    [this, token](const f1dl::String&) {
                      agent_stopped_.Pass();

                      // Send a message to the stopped agent which should
                      // trigger it.
                      modular::MessageSenderPtr message_sender;
                      component_context_->GetMessageSender(
                          token, message_sender.NewRequest());
                      message_sender->Send("Time to wake up...");

                      modular::testing::GetStore()->Get(
                          "trigger_test_agent_run_task",
                          [this](const f1dl::String&) {
                            task_triggered_.Pass();

                            modular::testing::GetStore()->Get(
                                "trigger_test_agent_stopped",
                                [this](const f1dl::String&) {
                                  module_host_->module_context()->Done();
                                });
                          });
                    });
              });
        });

    // Start a timer to quit in case another test component misbehaves and we
    // time out.
    fsl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
        callback::MakeScoped(
            weak_ptr_factory_.GetWeakPtr(),
            [this] { module_host_->module_context()->Done(); }),
        fxl::TimeDelta::FromMilliseconds(kTimeoutMilliseconds));
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
  modular::TriggerTestServicePtr agent_service_;
  modular::ComponentContextPtr component_context_;
  modular::MessageQueuePtr msg_queue_;
  fxl::WeakPtrFactory<ParentApp> weak_ptr_factory_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = app::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<ParentApp> driver(app_context.get(),
                                          [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
