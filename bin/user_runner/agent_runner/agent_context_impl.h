// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_USER_RUNNER_AGENT_RUNNER_AGENT_CONTEXT_IMPL_H_
#define PERIDOT_BIN_USER_RUNNER_AGENT_RUNNER_AGENT_CONTEXT_IMPL_H_

#include <string>

#include <component/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <modular_auth/cpp/fidl.h>
#include "lib/app/cpp/service_provider_impl.h"
#include "lib/async/cpp/operation.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/user_runner/component_context_impl.h"
#include "peridot/lib/fidl/app_client.h"

namespace fuchsia {
namespace modular {

class AgentRunner;

// The parameters of agent context that do not vary by instance.
struct AgentContextInfo {
  const ComponentContextInfo component_context_info;
  component::ApplicationLauncher* const app_launcher;
  modular_auth::TokenProviderFactory* const token_provider_factory;
  UserIntelligenceProvider* const user_intelligence_provider;
};

// This class manages an agent and its life cycle. AgentRunner owns this class,
// and instantiates one for every instance of an agent running. All requests for
// this agent (identified for now by the agent's URL) are routed to this
// class. This class manages all AgentControllers associated with this agent.
class AgentContextImpl : AgentContext, AgentController {
 public:
  explicit AgentContextImpl(const AgentContextInfo& info,
                            AppConfig agent_config);
  ~AgentContextImpl() override;

  // Stops the running agent, irrespective of whether there are active
  // AgentControllers or outstanding tasks. Calls into
  // |AgentRunner::RemoveAgent()| to remove itself.
  void StopForTeardown();

  // Called by AgentRunner when a component wants to connect to this agent.
  // Connections will pend until Agent::Initialize() responds back, at which
  // point all connections will be forwarded to the agent.
  void NewAgentConnection(
      const std::string& requestor_url,
      fidl::InterfaceRequest<component::ServiceProvider>
          incoming_services_request,
      fidl::InterfaceRequest<AgentController> agent_controller_request);

  // Called by AgentRunner when the framework wants to talk to the
  // |EntityProvider| service from this agent. Similar to NewAgentConnection(),
  // this operation will pend until the entity provider agent is initialized.
  void NewEntityProviderConnection(
      fidl::InterfaceRequest<EntityProvider> entity_provider_request,
      fidl::InterfaceRequest<AgentController> agent_controller_request);

  // Called by AgentRunner when a new task has been scheduled.
  void NewTask(const std::string& task_id);

  enum class State { INITIALIZING, RUNNING, TERMINATING };
  State state() { return state_; }

 private:
  // |AgentContext|
  void GetComponentContext(
      fidl::InterfaceRequest<ComponentContext> request) override;
  // |AgentContext|
  void GetTokenProvider(
      fidl::InterfaceRequest<modular_auth::TokenProvider> request) override;
  // |AgentContext|
  void ScheduleTask(TaskInfo task_info) override;
  // |AgentContext|
  void DeleteTask(fidl::StringPtr task_id) override;
  // |AgentContext|
  void GetIntelligenceServices(
      fidl::InterfaceRequest<IntelligenceServices> request) override;
  // |AgentContext|
  void GetEntityReferenceFactory(
      fidl::InterfaceRequest<EntityReferenceFactory> request) override;

  // Adds an operation on |operation_queue_|. This operation is immediately
  // Done() if this agent is not |ready_|. Else if there are no active
  // AgentControllers and no outstanding task, Agent.Stop() is called with a
  // timeout.
  void MaybeStopAgent();

  const std::string url_;

  std::unique_ptr<AppClient<Lifecycle>> app_client_;
  AgentPtr agent_;
  fidl::BindingSet<AgentContext> agent_context_bindings_;
  fidl::BindingSet<AgentController> agent_controller_bindings_;

  AgentRunner* const agent_runner_;

  ComponentContextImpl component_context_impl_;

  // A service provider that represents the services to be added into an
  // application's namespace.
  component::ServiceProviderImpl service_provider_impl_;

  modular_auth::TokenProviderFactory* const
      token_provider_factory_;                                  // Not owned.
  EntityProviderRunner* const entity_provider_runner_;          // Not owned.
  UserIntelligenceProvider* const user_intelligence_provider_;  // Not owned.

  State state_ = State::INITIALIZING;

  // Number of times Agent.RunTask() was called but we're still waiting on its
  // completion callback.
  int incomplete_task_count_ = 0;

  OperationQueue operation_queue_;

  // Operations implemented here.
  class InitializeCall;
  class StopCall;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentContextImpl);
};

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_BIN_USER_RUNNER_AGENT_RUNNER_AGENT_CONTEXT_IMPL_H_
