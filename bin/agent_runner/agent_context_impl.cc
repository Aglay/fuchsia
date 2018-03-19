// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/agent_runner/agent_context_impl.h"

#include <memory>

#include "lib/entity/fidl/entity_reference_factory.fidl.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "peridot/bin/agent_runner/agent_runner.h"
#include "peridot/lib/common/teardown.h"

namespace modular {

constexpr char kAppStoragePath[] = "/data/APP_DATA";

namespace {

// A stopgap solution to map an agent's url to a directory name where the
// agent's /data is mapped. We need three properties here - (1) two module urls
// that are the same get mapped to the same hash, (2) two modules urls that are
// different don't get the same name (with very high probability) and (3) the
// name is visually inspectable.
std::string HashAgentUrl(const std::string& agent_url) {
  std::size_t found = agent_url.find_last_of('/');
  auto last_part =
      found == agent_url.length() - 1 ? "" : agent_url.substr(found + 1);
  return std::to_string(std::hash<std::string>{}(agent_url)) + last_part;
}

};  // namespace

class AgentContextImpl::InitializeCall : Operation<> {
 public:
  InitializeCall(OperationContainer* const container,
                 AgentContextImpl* const agent_context_impl,
                 component::ApplicationLauncher* const app_launcher,
                 AppConfigPtr agent_config)
      : Operation("AgentContextImpl::InitializeCall", container, [] {},
                  agent_context_impl->url_),
        agent_context_impl_(agent_context_impl),
        app_launcher_(app_launcher),
        agent_config_(std::move(agent_config)) {
    Ready();
  }

 private:
  void Run() override {
    FXL_CHECK(agent_context_impl_->state_ == State::INITIALIZING);

    FlowToken flow{this};

    // No user intelligence provider is available during testing. We want to
    // keep going without it.
    if (!agent_context_impl_->user_intelligence_provider_) {
      auto service_list = component::ServiceList::New();
      Continue(std::move(service_list), flow);
      return;
    }

    agent_context_impl_->user_intelligence_provider_->GetServicesForAgent(
        agent_context_impl_->url_,
        [this, flow](component::ServiceListPtr maxwell_service_list) {
          auto service_list = component::ServiceList::New();
          service_list->names = std::move(maxwell_service_list->names);
          agent_context_impl_->service_provider_impl_.SetDefaultServiceProvider(
              maxwell_service_list->provider.Bind());
          Continue(std::move(service_list), flow);
        });
  }

  void Continue(component::ServiceListPtr service_list, FlowToken flow) {
    service_list->names.push_back(AgentContext::Name_);
    agent_context_impl_->service_provider_impl_.AddBinding(
        service_list->provider.NewRequest());
    agent_context_impl_->app_client_ = std::make_unique<AppClient<Lifecycle>>(
        app_launcher_, std::move(agent_config_),
        std::string(kAppStoragePath) + HashAgentUrl(agent_context_impl_->url_),
        std::move(service_list));

    agent_context_impl_->app_client_->services().ConnectToService(
        agent_context_impl_->agent_.NewRequest());

    // We only want to use Lifecycle if it exists.
    agent_context_impl_->app_client_->primary_service().set_error_handler(
        [agent_context_impl = agent_context_impl_] {
          agent_context_impl->app_client_->primary_service().Unbind();
        });

    // When the agent process dies, we remove it.
    // TODO(alhaad): In the future we would want to detect a crashing agent and
    // stop scheduling tasks for it.
    agent_context_impl_->app_client_->SetAppErrorHandler(
        [agent_context_impl = agent_context_impl_] {
          agent_context_impl->agent_runner_->RemoveAgent(
              agent_context_impl->url_);
        });

    // When all the |AgentController| bindings go away maybe stop the agent.
    agent_context_impl_->agent_controller_bindings_.set_empty_set_handler(
        [agent_context_impl = agent_context_impl_] {
          agent_context_impl->MaybeStopAgent();
        });

    agent_context_impl_->state_ = State::RUNNING;
  }

  AgentContextImpl* const agent_context_impl_;
  component::ApplicationLauncher* const app_launcher_;
  AppConfigPtr agent_config_;

  FXL_DISALLOW_COPY_AND_ASSIGN(InitializeCall);
};

// If |is_terminating| is set to true, the agent will be torn down irrespective
// of whether there is an open-connection or running task. Returns |true| if the
// agent was stopped, false otherwise (could be because agent has pending
// tasks).
class AgentContextImpl::StopCall : Operation<bool> {
 public:
  StopCall(OperationContainer* const container,
           const bool terminating,
           AgentContextImpl* const agent_context_impl,
           ResultCall result_call)
      : Operation("AgentContextImpl::StopCall",
                  container,
                  std::move(result_call),
                  agent_context_impl->url_),
        agent_context_impl_(agent_context_impl),
        terminating_(terminating) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &stopped_};

    if (agent_context_impl_->state_ == State::TERMINATING) {
      return;
    }

    if (terminating_ ||
        (agent_context_impl_->agent_controller_bindings_.size() == 0 &&
         agent_context_impl_->incomplete_task_count_ == 0)) {
      Stop(flow);
    }
  }

  void Stop(FlowToken flow) {
    agent_context_impl_->state_ = State::TERMINATING;
    agent_context_impl_->app_client_->Teardown(kBasicTimeout,
                                               [this, flow] { Kill(flow); });
  }

  void Kill(FlowToken flow) {
    stopped_ = true;
    agent_context_impl_->agent_.Unbind();
    agent_context_impl_->agent_context_bindings_.CloseAll();
  }

  bool stopped_ = false;
  AgentContextImpl* const agent_context_impl_;
  const bool terminating_;  // is the agent runner terminating?

  FXL_DISALLOW_COPY_AND_ASSIGN(StopCall);
};

AgentContextImpl::AgentContextImpl(const AgentContextInfo& info,
                                   AppConfigPtr agent_config)
    : url_(agent_config->url),
      agent_runner_(info.component_context_info.agent_runner),
      component_context_impl_(info.component_context_info,
                              kAgentComponentNamespace,
                              url_,
                              url_),
      token_provider_factory_(info.token_provider_factory),
      entity_provider_runner_(
          info.component_context_info.entity_provider_runner),
      user_intelligence_provider_(info.user_intelligence_provider) {
  service_provider_impl_.AddService<AgentContext>(
      [this](f1dl::InterfaceRequest<AgentContext> request) {
        agent_context_bindings_.AddBinding(this, std::move(request));
      });
  new InitializeCall(&operation_queue_, this, info.app_launcher,
                     std::move(agent_config));
}

AgentContextImpl::~AgentContextImpl() = default;

void AgentContextImpl::NewAgentConnection(
    const std::string& requestor_url,
    f1dl::InterfaceRequest<component::ServiceProvider>
        incoming_services_request,
    f1dl::InterfaceRequest<AgentController> agent_controller_request) {
  // Queue adding the connection
  new SyncCall(
      &operation_queue_,
      fxl::MakeCopyable([this, requestor_url,
                         incoming_services_request =
                             std::move(incoming_services_request),
                         agent_controller_request =
                             std::move(agent_controller_request)]() mutable {
        FXL_CHECK(state_ == State::RUNNING);

        agent_->Connect(requestor_url, std::move(incoming_services_request));

        // Add a binding to the |controller|. When all the bindings go away,
        // the agent will stop.
        agent_controller_bindings_.AddBinding(
            this, std::move(agent_controller_request));
      }));
}

void AgentContextImpl::NewEntityProviderConnection(
    f1dl::InterfaceRequest<EntityProvider> entity_provider_request,
    f1dl::InterfaceRequest<AgentController> agent_controller_request) {
  new SyncCall(
      &operation_queue_,
      fxl::MakeCopyable(
          [this, entity_provider_request = std::move(entity_provider_request),
           agent_controller_request =
               std::move(agent_controller_request)]() mutable {
            FXL_CHECK(state_ == State::RUNNING);
            app_client_->services().ConnectToService(
                std::move(entity_provider_request));
            agent_controller_bindings_.AddBinding(
                this, std::move(agent_controller_request));
          }));
}

void AgentContextImpl::NewTask(const std::string& task_id) {
  new SyncCall(&operation_queue_, [this, task_id] {
    FXL_CHECK(state_ == State::RUNNING);
    // Increment the counter for number of incomplete tasks. Decrement it when
    // we receive its callback;
    incomplete_task_count_++;
    agent_->RunTask(task_id, [this] {
      incomplete_task_count_--;
      MaybeStopAgent();
    });
  });
}

void AgentContextImpl::GetComponentContext(
    f1dl::InterfaceRequest<ComponentContext> request) {
  component_context_impl_.Connect(std::move(request));
}

void AgentContextImpl::GetTokenProvider(
    f1dl::InterfaceRequest<auth::TokenProvider> request) {
  token_provider_factory_->GetTokenProvider(url_, std::move(request));
}

void AgentContextImpl::GetIntelligenceServices(
    f1dl::InterfaceRequest<maxwell::IntelligenceServices> request) {
  auto agent_scope = maxwell::AgentScope::New();
  agent_scope->url = url_;
  auto scope = maxwell::ComponentScope::New();
  scope->set_agent_scope(std::move(agent_scope));
  user_intelligence_provider_->GetComponentIntelligenceServices(
      std::move(scope), std::move(request));
}

void AgentContextImpl::GetEntityReferenceFactory(
    f1dl::InterfaceRequest<EntityReferenceFactory> request) {
  entity_provider_runner_->ConnectEntityReferenceFactory(url_,
                                                         std::move(request));
}

void AgentContextImpl::ScheduleTask(TaskInfoPtr task_info) {
  agent_runner_->ScheduleTask(url_, std::move(task_info));
}

void AgentContextImpl::DeleteTask(const f1dl::StringPtr& task_id) {
  agent_runner_->DeleteTask(url_, task_id);
}

void AgentContextImpl::Done() {}

void AgentContextImpl::MaybeStopAgent() {
  new StopCall(&operation_queue_, false /* is agent runner terminating? */,
               this, [this](bool stopped) {
                 if (stopped) {
                   agent_runner_->RemoveAgent(url_);
                   // |this| is no longer valid at this point.
                 }
               });
}

void AgentContextImpl::StopForTeardown() {
  FXL_DLOG(INFO) << "AgentContextImpl::StopForTeardown() " << url_;
  new StopCall(&operation_queue_, true /* is agent runner terminating? */, this,
               [this](bool stopped) {
                 FXL_DCHECK(stopped);
                 agent_runner_->RemoveAgent(url_);
                 // |this| is no longer valid at this point.
               });
}

}  // namespace modular
