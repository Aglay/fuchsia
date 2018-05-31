// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <simple/cpp/fidl.h>

#include "lib/app/cpp/connect.h"
#include "lib/app/cpp/startup_context.h"
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fxl/functional/make_copyable.h"
#include "peridot/lib/fidl/message_receiver_client.h"

namespace simple {

class SimpleModule : fuchsia::ui::views_v1::ViewProvider {
 public:
  SimpleModule(fuchsia::modular::ModuleHost* module_host,
               fidl::InterfaceRequest<fuchsia::ui::views_v1::ViewProvider>
                   view_provider_request)
      : view_provider_binding_(this) {
    view_provider_binding_.Bind(std::move(view_provider_request));

    // Get the component context from the module context.
    fuchsia::modular::ComponentContextPtr component_context;
    module_host->module_context()->GetComponentContext(
        component_context.NewRequest());

    // Connect to the agent to retrieve it's outgoing services.
    fuchsia::modular::AgentControllerPtr agent_controller;
    fuchsia::sys::ServiceProviderPtr agent_services;
    component_context->ConnectToAgent("system/bin/simple_agent",
                                      agent_services.NewRequest(),
                                      agent_controller.NewRequest());

    // Connect to the SimpleService in the agent's services.
    SimplePtr agent_service;
    fuchsia::sys::ConnectToService(agent_services.get(),
                                agent_service.NewRequest());

    // Request a new message queue from the component context.
    fuchsia::modular::MessageQueuePtr message_queue;
    component_context->ObtainMessageQueue("agent_queue",
                                          message_queue.NewRequest());

    // Register a callback with a message receiver client that logs any
    // messages that SimpleAgent sends.
    message_receiver_ =
        std::make_unique<fuchsia::modular::MessageReceiverClient>(
            message_queue.get(),
            [](fidl::StringPtr msg, std::function<void()> ack) {
              ack();
              FXL_LOG(INFO) << "new message: " << msg;
            });

    // Get the token for the message queue and send it to the agent.
    message_queue->GetToken(fxl::MakeCopyable(
        [agent_service = std::move(agent_service)](fidl::StringPtr token) {
          agent_service->SetMessageQueue(token);
        }));
    FXL_LOG(INFO) << "Initialized Simple Module.";
  }

  // Called by ModuleDriver.
  void Terminate(const std::function<void()>& done) { done(); }

 private:
  // |fuchsia::ui::views_v1::ViewProvider|
  void CreateView(
      fidl::InterfaceRequest<fuchsia::ui::views_v1_token::ViewOwner> view_owner,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> services) override {}

  fidl::Binding<fuchsia::ui::views_v1::ViewProvider> view_provider_binding_;

  std::unique_ptr<fuchsia::modular::MessageReceiverClient> message_receiver_;
};

}  // namespace simple

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigMakeDefault);
  auto context = fuchsia::sys::StartupContext::CreateFromStartupInfo();
  fuchsia::modular::ModuleDriver<simple::SimpleModule> driver(
      context.get(), [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
