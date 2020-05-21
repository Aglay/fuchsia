// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/agent_runner/agent_context_impl.h"

#include <fuchsia/intl/cpp/fidl.h>
#include <fuchsia/io/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/vfs.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include <memory>

#include "lib/fdio/directory.h"
#include "src/modular/bin/sessionmgr/agent_runner/agent_runner.h"
#include "src/modular/lib/common/teardown.h"

namespace modular {

namespace {

// Get a list of names of the entries in a directory.
void GetFidlDirectoryEntries(fuchsia::io::Directory* dir,
                             fit::function<void(std::vector<std::string>)> callback) {
  constexpr uint64_t max_bytes = 4096;

  dir->ReadDirents(
      max_bytes, [callback = std::move(callback)](int32_t status, std::vector<uint8_t> dirents) {
        std::vector<std::string> entry_names{};

        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "GetFidlDirectoryEntries: could not read directory entries, error "
                         << status << " (" << zx_status_get_string(status) << ")";
          callback(std::move(entry_names));
          return;
        }

        uint64_t offset = 0;
        auto* data_ptr = dirents.data();
        while (dirents.size() - offset >= sizeof(vdirent_t)) {
          vdirent_t* de = reinterpret_cast<vdirent_t*>(data_ptr + offset);
          auto name = std::string(de->name, de->size);
          if (name.at(0) != '.') {
            entry_names.push_back(name);
          }
          offset += sizeof(vdirent_t) + de->size;
        }

        callback(std::move(entry_names));
      });
}

};  // namespace

class AgentContextImpl::InitializeCall : public Operation<> {
 public:
  InitializeCall(AgentContextImpl* const agent_context_impl, fuchsia::sys::Launcher* const launcher,
                 fuchsia::modular::AppConfig agent_config)
      : Operation(
            "AgentContextImpl::InitializeCall", [] {}, agent_context_impl->url_),
        agent_context_impl_(agent_context_impl),
        launcher_(launcher),
        agent_config_(std::move(agent_config)) {}

 private:
  void Run() override {
    FX_CHECK(agent_context_impl_->state_ == State::INITIALIZING);

    FlowToken flow{this};

    // No agent services factory is available during testing. We want to
    // keep going without it.
    if (!agent_context_impl_->agent_services_factory_) {
      auto service_list = fuchsia::sys::ServiceList::New();
      Continue(std::move(service_list), flow);
      return;
    }

    auto agent_service_list = agent_context_impl_->agent_services_factory_->GetServicesForAgent(
        agent_context_impl_->url_);
    auto service_list = fuchsia::sys::ServiceList::New();
    service_list->names = std::move(agent_service_list.names);
    agent_context_impl_->service_provider_impl_.SetDefaultServiceProvider(
        agent_service_list.provider.Bind());
    Continue(std::move(service_list), flow);
  }

  void Continue(fuchsia::sys::ServiceListPtr service_list, FlowToken flow) {
    service_list->names.push_back(fuchsia::modular::ComponentContext::Name_);
    service_list->names.push_back(fuchsia::modular::AgentContext::Name_);
    for (const auto& service_name : agent_context_impl_->agent_runner_->GetAgentServices()) {
      service_list->names.push_back(service_name);
    }

    auto agent_url = agent_config_.url;
    agent_context_impl_->service_provider_impl_.AddBinding(service_list->provider.NewRequest());
    agent_context_impl_->app_client_ = std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
        launcher_, std::move(agent_config_), /*data_origin=*/"", std::move(service_list));

    agent_context_impl_->app_client_->services().ConnectToService(
        agent_context_impl_->agent_.NewRequest());
    agent_context_impl_->agent_.set_error_handler([agent_url](zx_status_t status) {
      FX_PLOGS(INFO, status) << "Agent " << agent_url
                             << "closed its fuchsia.modular.Agent channel. "
                             << "This is expected for agents that don't expose it.";
    });

    // Enumerate the services that the agent has published in its outgoing directory.
    auto agent_outgoing_dir_handle =
        fdio_service_clone(agent_context_impl_->app_client_->services().directory().get());
    FX_CHECK(agent_outgoing_dir_handle != ZX_HANDLE_INVALID);
    zx::channel agent_outgoing_dir_chan(agent_outgoing_dir_handle);
    outgoing_dir_ptr_.Bind(std::move(agent_outgoing_dir_chan));

    GetFidlDirectoryEntries(outgoing_dir_ptr_.get(), [this, flow](auto entries) {
      agent_context_impl_->agent_outgoing_services_ = std::set<std::string>(
          std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    });

    // We only want to use fuchsia::modular::Lifecycle if it exists.
    agent_context_impl_->app_client_->primary_service().set_error_handler(
        [agent_context_impl = agent_context_impl_](zx_status_t status) {
          agent_context_impl->app_client_->primary_service().Unbind();
        });

    // When the agent component dies, clean up.
    agent_context_impl_->app_client_->SetAppErrorHandler(
        [agent_context_impl = agent_context_impl_] { agent_context_impl->StopOnAppError(); });

    // When all the |fuchsia::modular::AgentController| bindings go away maybe
    // stop the agent.
    agent_context_impl_->agent_controller_bindings_.set_empty_set_handler(
        [agent_context_impl = agent_context_impl_] { agent_context_impl->StopAgentIfIdle(); });

    agent_context_impl_->state_ = State::RUNNING;
  }

  AgentContextImpl* const agent_context_impl_;
  fuchsia::sys::Launcher* const launcher_;
  fuchsia::modular::AppConfig agent_config_;
  fuchsia::io::DirectoryPtr outgoing_dir_ptr_;
};

// If |is_teardown| is set to true, the agent will be torn down irrespective
// of whether there is an open-connection. Returns |true| if the
// agent was stopped, false otherwise.
class AgentContextImpl::StopCall : public Operation<bool> {
 public:
  StopCall(const bool is_teardown, AgentContextImpl* const agent_context_impl,
           ResultCall result_call)
      : Operation("AgentContextImpl::StopCall", std::move(result_call), agent_context_impl->url_),
        agent_context_impl_(agent_context_impl),
        is_teardown_(is_teardown) {}

 private:
  void Run() override {
    FlowToken flow{this, &stopped_};

    if (agent_context_impl_->state_ == State::TERMINATING ||
        agent_context_impl_->state_ == State::TERMINATED) {
      return;
    }

    // Don't stop the agent if it has connections, unless it's being torn down.
    if (!is_teardown_ && agent_context_impl_->agent_controller_bindings_.size() != 0) {
      return;
    }

    // If there's no fuchsia::modular::Lifecycle binding, it's not possible to teardown gracefully.
    if (!agent_context_impl_->app_client_->primary_service().is_bound()) {
      Stop(flow);
    } else {
      Teardown(flow);
    }
  }

  void Teardown(FlowToken flow) {
    FlowTokenHolder branch{flow};

    agent_context_impl_->state_ = State::TERMINATING;

    // Calling Teardown() below will branch |flow| into normal and timeout
    // paths. |flow| must go out of scope when either of the paths finishes.
    //
    // TODO(mesch): AppClient/AsyncHolder should implement this. See also
    // StoryProviderImpl::StopStoryShellCall.
    agent_context_impl_->app_client_->Teardown(kBasicTimeout, [this, branch] {
      std::unique_ptr<FlowToken> cont = branch.Continue();
      if (cont) {
        Stop(*cont);
      }
    });
  }

  void Stop(FlowToken flow) {
    stopped_ = true;
    agent_context_impl_->state_ = State::TERMINATED;
    agent_context_impl_->agent_.Unbind();
    agent_context_impl_->agent_context_bindings_.CloseAll();
    agent_context_impl_->token_manager_bindings_.CloseAll();
    agent_context_impl_->app_client_.reset();
  }

  bool stopped_ = false;
  AgentContextImpl* const agent_context_impl_;
  const bool is_teardown_;
};

class AgentContextImpl::OnAppErrorCall : public Operation<> {
 public:
  OnAppErrorCall(AgentContextImpl* const agent_context_impl, ResultCall result_call)
      : Operation("AgentContextImpl::OnAppErrorCall", std::move(result_call),
                  agent_context_impl->url_),
        agent_context_impl_(agent_context_impl) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // The agent is already being cleanly terminated. |StopCall| will clean up.
    if (agent_context_impl_->state_ == State::TERMINATING) {
      return;
    }

    agent_context_impl_->state_ = State::TERMINATED;
    agent_context_impl_->agent_.Unbind();
    agent_context_impl_->agent_context_bindings_.CloseAll();
    agent_context_impl_->token_manager_bindings_.CloseAll();
    agent_context_impl_->app_client_.reset();
  }

  AgentContextImpl* const agent_context_impl_;
};

AgentContextImpl::AgentContextImpl(const AgentContextInfo& info,
                                   fuchsia::modular::AppConfig agent_config,
                                   inspect::Node agent_node)
    : url_(agent_config.url),
      component_context_impl_(info.component_context_info, url_, url_),
      agent_runner_(info.component_context_info.agent_runner),
      agent_services_factory_(info.agent_services_factory),
      agent_node_(std::move(agent_node)) {
  agent_runner_->PublishAgentServices(url_, &service_provider_impl_);
  service_provider_impl_.AddService<fuchsia::modular::ComponentContext>(
      [this](fidl::InterfaceRequest<fuchsia::modular::ComponentContext> request) {
        component_context_impl_.Connect(std::move(request));
      });
  service_provider_impl_.AddService<fuchsia::modular::AgentContext>(
      [this](fidl::InterfaceRequest<fuchsia::modular::AgentContext> request) {
        agent_context_bindings_.AddBinding(this, std::move(request));
      });
  if (info.sessionmgr_context != nullptr) {
    service_provider_impl_.AddService<fuchsia::intl::PropertyProvider>(
        [info](fidl::InterfaceRequest<fuchsia::intl::PropertyProvider> request) {
          info.sessionmgr_context->svc()->Connect<fuchsia::intl::PropertyProvider>(
              std::move(request));
        });
  }
  operation_queue_.Add(
      std::make_unique<InitializeCall>(this, info.launcher, std::move(agent_config)));
}

AgentContextImpl::~AgentContextImpl() = default;

void AgentContextImpl::ConnectToService(
    std::string requestor_url,
    fidl::InterfaceRequest<fuchsia::modular::AgentController> agent_controller_request,
    std::string service_name, ::zx::channel channel) {
  // Run this task on the operation queue to ensure that all member variables are
  // fully initialized before we query their state.
  operation_queue_.Add(std::make_unique<SyncCall>(
      [this, requestor_url, agent_controller_request = std::move(agent_controller_request),
       service_name, channel = std::move(channel)]() mutable {
        FX_CHECK(state_ == State::RUNNING);

        if (agent_outgoing_services_.count(service_name) > 0) {
          app_client_->services().ConnectToService(std::move(channel), service_name);
        } else if (agent_.is_bound()) {
          fuchsia::sys::ServiceProviderPtr agent_services;
          agent_->Connect(requestor_url, agent_services.NewRequest());
          agent_services->ConnectToService(service_name, std::move(channel));
        }

        // Add a binding to the |controller|. When all the bindings go away,
        // the agent will stop.
        agent_controller_bindings_.AddBinding(this, std::move(agent_controller_request));
      }));
}

void AgentContextImpl::NewAgentConnection(
    const std::string& requestor_url,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services_request,
    fidl::InterfaceRequest<fuchsia::modular::AgentController> agent_controller_request) {
  // Queue adding the connection
  operation_queue_.Add(std::make_unique<SyncCall>(
      [this, requestor_url, incoming_services_request = std::move(incoming_services_request),
       agent_controller_request = std::move(agent_controller_request)]() mutable {
        FX_CHECK(state_ == State::RUNNING);

        if (agent_.is_bound()) {
          agent_->Connect(requestor_url, std::move(incoming_services_request));
        }

        // Add a binding to the |controller|. When all the bindings go away,
        // the agent will stop.
        agent_controller_bindings_.AddBinding(this, std::move(agent_controller_request));
      }));
}

void AgentContextImpl::GetComponentContext(
    fidl::InterfaceRequest<fuchsia::modular::ComponentContext> request) {
  component_context_impl_.Connect(std::move(request));
}

void AgentContextImpl::GetTokenManager(
    fidl::InterfaceRequest<fuchsia::auth::TokenManager> request) {
  token_manager_bindings_.AddBinding(this, std::move(request));
}

void AgentContextImpl::Authorize(
    fuchsia::auth::AppConfig app_config,
    fidl::InterfaceHandle<fuchsia::auth::AuthenticationUIContext> auth_ui_context,
    std::vector<::std::string> app_scopes, fidl::StringPtr user_profile_id,
    fidl::StringPtr auth_code, AuthorizeCallback callback) {
  FX_LOGS(ERROR) << "AgentContextImpl::Authorize() not supported from agent "
                 << "context";
  callback(fuchsia::auth::Status::INVALID_REQUEST, nullptr);
}

void AgentContextImpl::GetAccessToken(fuchsia::auth::AppConfig app_config,
                                      std::string user_profile_id,
                                      std::vector<::std::string> app_scopes,
                                      GetAccessTokenCallback callback) {
  FX_LOGS(ERROR) << "AgentContextImpl::GetAccessToken() not supported from "
                 << "agent context";
  callback(fuchsia::auth::Status::INVALID_REQUEST, nullptr);
}

void AgentContextImpl::GetIdToken(fuchsia::auth::AppConfig app_config, std::string user_profile_id,
                                  fidl::StringPtr audience, GetIdTokenCallback callback) {
  FX_LOGS(ERROR) << "AgentContextImpl::GetIdToken() not supported from agent "
                 << "context";
  callback(fuchsia::auth::Status::INVALID_REQUEST, nullptr);
}

void AgentContextImpl::DeleteAllTokens(fuchsia::auth::AppConfig app_config,
                                       std::string user_profile_id, bool force,
                                       DeleteAllTokensCallback callback) {
  FX_LOGS(ERROR) << "AgentContextImpl::DeleteAllTokens() not supported from "
                 << "agent context";
  callback(fuchsia::auth::Status::INVALID_REQUEST);
}

void AgentContextImpl::ListProfileIds(fuchsia::auth::AppConfig app_config,
                                      ListProfileIdsCallback callback) {
  FX_LOGS(ERROR) << "AgentContextImpl::ListProfileIds() not supported from "
                 << "agent context";
  callback(fuchsia::auth::Status::INVALID_REQUEST, {});
}

void AgentContextImpl::StopAgentIfIdle() {
  // See if this agent is in the agent service index. If so, and to facilitate components
  // with connections to the agent made through the environment and without associated
  // AgentControllers, short-circuit the usual idle cleanup and leave us running.
  if (agent_runner_->AgentInServiceIndex(url_)) {
    return;
  }

  operation_queue_.Add(
      std::make_unique<StopCall>(/*is_teardown=*/false, this, [this](bool stopped) {
        if (stopped) {
          agent_runner_->RemoveAgent(url_);
          // |this| is no longer valid at this point.
        }
      }));
}

void AgentContextImpl::StopForTeardown(fit::function<void()> callback) {
  FX_LOGS(INFO) << "AgentContextImpl::StopForTeardown() " << url_;

  operation_queue_.Add(std::make_unique<StopCall>(
      /*is_teardown=*/true, this, [this, callback = std::move(callback)](bool stopped) {
        FX_DCHECK(stopped);
        agent_runner_->RemoveAgent(url_);
        callback();
        // |this| is no longer valid at this point.
      }));
}

void AgentContextImpl::StopOnAppError() {
  operation_queue_.Add(std::make_unique<OnAppErrorCall>(this, [this]() {
    agent_runner_->RemoveAgent(url_);
    // |this| is no longer valid at this point.
  }));
}

}  // namespace modular
