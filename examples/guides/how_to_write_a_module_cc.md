# How-To: Write a Module in C++

## Overview

A `Module` is a UI component that can participate in a [Story](link to story doc), 
potentially composed of many different `Module`s. A `Module`'s lifecycle is tightly
bound to the story to which it was added. In addition to the capabilities
provided to all Peridot components via `ComponentContext`, a `Module` is given
additional capabilities via its `ModuleContext`.

## `SimpleMod`

`SimpleMod` is a `Module` communicates with `SimpleAgent` via a `MessageQueue`, and
displays the messages from `SimpleAgent` on screen.

### Mod Initialization

The first step to writing a `Module` is implementing the initializer.

```c++
#include <ui/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/module_driver.h"

namespace simple {

class SimpleModule : views_v1::ViewProvider {
 public:
	SimpleModule(
			modular::ModuleHost* module_host,
			fidl::InterfaceRequest<views_v1::ViewProvider> view_provider_request)
			: view_provider_binding_(this) {
		view_provider_binding_.Bind(std::move(view_provider_request));
}

 private:
	modular::ModuleHost* module_host_;
	fidl::Binding<views_v1::ViewProvider> view_provider_binding_;
	std::set<std::unique_ptr<SimpleView>> views_;
};

}  // namespace simple
```

The `ModuleHost` provides `SimpleModule` with its `ApplicationContext` and
`ModuleContext`.

The `ViewProvider` request allows the system to connect to `SimpleModule`'s view.
TODO: Update guide to explain view connections.

### Connecting to `SimpleAgent`

In order to provide `SimpleAgent` with a message queue `SimpleModule` first
needs to connect to the agent via its `ComponentContext`.

```c++
// Get the component context from the module context.
modular::ComponentContextPtr component_context;
module_host->module_context()->GetComponentContext(
    component_context.NewRequest());

// Connect to the agent to retrieve it's outgoing services.
modular::AgentControllerPtr agent_controller;
component::ServiceProviderPtr agent_services;
component_context->ConnectToAgent("system/bin/simple_agent",
                                  agent_services.NewRequest(),
                                  agent_controller.NewRequest());
```

### Creating a `MessageQueue`

`SimpleModule` needs to create a message queue and retrieve its token to hand
it over to `SimpleAgent` so it can write messages to it.

```c++
// Request a new message queue from the component context.
modular::MessageQueuePtr message_queue;
component_context->ObtainMessageQueue("agent_queue",
                                      message_queue.NewRequest());

// Get the token for the message queue and send it to the agent.
message_queue->GetToken(fxl::MakeCopyable(
    [agent_service = std::move(agent_service)](fidl::StringPtr token) {
      agent_service->SetMessageQueue(token);
    }));
```

### Communicating with `SimpleAgent

In order to observe messages on the newly created message queue, `SimpleModule`
uses a `MessageReceiverClient`. The client takes a lambda that gets called with
new messages.

```c++
// Register a callback with a message receiver client that logs any
// messages that SimpleAgent sends.
message_receiver_ = std::make_unique<modular::MessageReceiverClient>(
    message_queue.get(),
    [](fidl::StringPtr msg, std::function<void()> ack) {
      ack();  
      FXL_LOG(INFO) << "New message: " << msg;
    });
```

### Running the Module

```c++
int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigMakeDefault);
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<simple::SimpleModule> driver(app_context.get(),
                                                     [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
```

`ModuleDriver` is a helper class that manages the `Module`'s lifecyle. Here it is
given a newly created `ApplicationContext` and a callback that will be executed
when the `Module` exits. `ModuleDriver` requires `SimpleModule` to implement the
constructor shown above, as well as a `Terminate`:

```c++
void Terminate(const std::function<void()>& done);
```

The module is responsible for calling `done` once its shutdown sequence is complete.

