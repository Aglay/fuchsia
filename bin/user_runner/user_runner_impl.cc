// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/user_runner_impl.h"

#include <memory>
#include <string>

#include "lib/agent/fidl/agent_provider.fidl.h"
#include "lib/clipboard/fidl/clipboard.fidl.h"
#include "lib/config/fidl/config.fidl.h"
#include "lib/fxl/files/directory.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "lib/network/fidl/network_service.fidl.h"
#include "lib/resolver/fidl/resolver.fidl.h"
#include "lib/story/fidl/story_provider.fidl.h"
#include "lib/suggestion/fidl/suggestion_provider.fidl.h"
#include "lib/ui/views/fidl/view_provider.fidl.h"
#include "lib/ui/views/fidl/view_token.fidl.h"
#include "lib/user/fidl/user_runner.fidl.h"
#include "lib/user/fidl/user_shell.fidl.h"
#include "lib/user_intelligence/fidl/user_intelligence_provider.fidl.h"
#include "peridot/bin/cloud_provider_firebase/fidl/factory.fidl.h"
#include "peridot/bin/component/component_context_impl.h"
#include "peridot/bin/component/message_queue_manager.h"
#include "peridot/bin/device_runner/cobalt/cobalt.h"
#include "peridot/bin/ledger/fidl/debug.fidl.h"
#include "peridot/bin/story_runner/link_impl.h"
#include "peridot/bin/story_runner/story_provider_impl.h"
#include "peridot/bin/user_runner/device_map_impl.h"
#include "peridot/bin/user_runner/focus.h"
#include "peridot/bin/user_runner/remote_invoker_impl.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/common/xdr.h"
#include "peridot/lib/device_info/device_info.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/fidl/scope.h"
#include "peridot/lib/ledger_client/constants.h"
#include "peridot/lib/ledger_client/ledger_client.h"
#include "peridot/lib/ledger_client/status.h"
#include "peridot/lib/ledger_client/storage.h"

namespace modular {

// Maxwell doesn't yet implement lifecycle or has a lifecycle method, so we just
// let AppClient close the controller connection immediately. (The controller
// connection is closed once the ServiceTerminate() call invokes its done
// callback.)
template <>
void AppClient<maxwell::UserIntelligenceProviderFactory>::ServiceTerminate(
    const std::function<void()>& done) {
  done();
}

namespace {

constexpr char kAppId[] = "modular_user_runner";
constexpr char kMaxwellComponentNamespace[] = "maxwell";
constexpr char kMaxwellUrl[] = "maxwell";
constexpr char kContextEngineUrl[] = "context_engine";
constexpr char kContextEngineComponentNamespace[] = "context_engine";
constexpr char kModuleResolverUrl[] = "module_resolver";
constexpr char kUserScopeLabelPrefix[] = "user-";
constexpr char kMessageQueuePath[] = "/data/MESSAGE_QUEUES/v1/";
constexpr char kUserShellComponentNamespace[] = "user-shell-namespace";
constexpr char kUserShellLinkName[] = "user-shell-link";
constexpr char kLedgerDashboardUrl[] = "ledger_dashboard";
constexpr char kLedgerDashboardEnvLabel[] = "ledger-dashboard";
constexpr char kClipboardAgentUrl[] = "file:///system/bin/agents/clipboard";

cloud_provider_firebase::ConfigPtr GetLedgerFirebaseConfig() {
  auto firebase_config = cloud_provider_firebase::Config::New();
  firebase_config->server_id = kFirebaseServerId;
  firebase_config->api_key = kFirebaseApiKey;
  return firebase_config;
}

std::string GetAccountId(const auth::AccountPtr& account) {
  return account.is_null() ? "GUEST" : account->id;
}

// Creates a function that can be used as termination action passed to AtEnd(),
// which when called invokes the reset() method on the object pointed to by the
// argument. Used to reset() fidl pointers and std::unique_ptr<>s fields.
template <typename X>
std::function<void(std::function<void()>)> Reset(
    std::unique_ptr<X>* const field) {
  return [field](std::function<void()> cont) {
    field->reset();
    cont();
  };
}

template <typename X>
std::function<void(std::function<void()>)> Reset(
    fidl::StructPtr<X>* const field) {
  return [field](std::function<void()> cont) {
    field->reset();
    cont();
  };
}

template <typename X>
std::function<void(std::function<void()>)> Reset(
    fidl::InterfacePtr<X>* const field) {
  return [field](std::function<void()> cont) {
    field->Unbind();
    cont();
  };
}

// Creates a function that can be used as termination action passed to AtEnd(),
// which when called asynchronously invokes the Teardown() method on the object
// pointed to by the argument. Used to teardown AppClient and AsyncHolder
// members.
template <typename X>
std::function<void(std::function<void()>)> Teardown(
    const fxl::TimeDelta timeout,
    const char* const message,
    X* const field) {
  return [timeout, message, field](std::function<void()> cont) {
    field->Teardown(timeout, [message, cont] {
      if (message) {
        FXL_DLOG(INFO) << "- " << message << " down.";
      }
      cont();
    });
  };
}

}  // namespace

UserRunnerImpl::UserRunnerImpl(
    app::ApplicationContext* const application_context,
    const bool test)
    : application_context_(application_context),
      test_(test),
      user_shell_context_binding_(this),
      story_provider_impl_("StoryProviderImpl"),
      agent_runner_("AgentRunner") {
  application_context_->outgoing_services()->AddService<UserRunner>(
      [this](fidl::InterfaceRequest<UserRunner> request) {
        bindings_.AddBinding(this, std::move(request));
      });

  // TODO(alhaad): Once VFS supports asynchronous operations, expose directly
  // to filesystem instead of this indirection.
  application_context_->outgoing_services()->AddService<UserRunnerDebug>(
      [this](fidl::InterfaceRequest<UserRunnerDebug> request) {
        user_runner_debug_bindings_.AddBinding(this, std::move(request));
      });
}

UserRunnerImpl::~UserRunnerImpl() = default;

void UserRunnerImpl::Initialize(
    auth::AccountPtr account,
    AppConfigPtr user_shell,
    AppConfigPtr story_shell,
    fidl::InterfaceHandle<auth::TokenProviderFactory> token_provider_factory,
    fidl::InterfaceHandle<UserContext> user_context,
    fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request) {
  InitializeUser(std::move(account), std::move(token_provider_factory),
                 std::move(user_context));
  InitializeLedger();
  InitializeLedgerDashboard();
  InitializeDeviceMap();
  InitializeRemoteInvoker();
  InitializeMessageQueueManager();
  InitializeMaxwell(user_shell->url, std::move(story_shell));
  InitializeClipboard();
  InitializeUserShell(std::move(user_shell), std::move(view_owner_request));
  ReportEvent(CobaltEvent::BOOTED_TO_USER_RUNNER);
}

void UserRunnerImpl::InitializeUser(
    auth::AccountPtr account,
    fidl::InterfaceHandle<auth::TokenProviderFactory> token_provider_factory,
    fidl::InterfaceHandle<UserContext> user_context) {
  token_provider_factory_ = token_provider_factory.Bind();
  AtEnd(Reset(&token_provider_factory_));

  user_context_ = user_context.Bind();
  AtEnd(Reset(&user_context_));

  account_ = std::move(account);
  AtEnd(Reset(&account_));

  user_scope_ = std::make_unique<Scope>(
      application_context_->environment(),
      std::string(kUserScopeLabelPrefix) + GetAccountId(account_));
  AtEnd(Reset(&user_scope_));
}

void UserRunnerImpl::InitializeLedger() {
  AppConfigPtr ledger_config = AppConfig::New();
  ledger_config->url = kLedgerAppUrl;
  ledger_config->args = fidl::Array<fidl::String>::New(1);
  ledger_config->args[0] = kLedgerNoMinfsWaitFlag;

  app::ServiceListPtr service_list = nullptr;
  if (account_) {
    service_list = app::ServiceList::New();
    service_list->names.push_back(auth::TokenProvider::Name_);
    ledger_service_provider_.AddService<auth::TokenProvider>(
        [this](fidl::InterfaceRequest<auth::TokenProvider> request) {
          token_provider_factory_->GetTokenProvider(kLedgerAppUrl,
                                                    std::move(request));
        });
    ledger_service_provider_.AddBinding(service_list->provider.NewRequest());
  }

  ledger_app_ = std::make_unique<AppClient<ledger::LedgerController>>(
      user_scope_->GetLauncher(), std::move(ledger_config), "/data/LEDGER",
      std::move(service_list));
  ledger_app_->SetAppErrorHandler([this] {
    FXL_LOG(ERROR) << "Ledger seems to have crashed unexpectedly." << std::endl
                   << "CALLING Logout() DUE TO UNRECOVERABLE LEDGER ERROR.";
    Logout();
  });
  AtEnd(Teardown(kBasicTimeout, "Ledger", ledger_app_.get()));

  cloud_provider::CloudProviderPtr cloud_provider;
  if (account_) {
    // If not running in Guest mode, spin up a cloud provider for Ledger to use
    // for syncing.
    AppConfigPtr cloud_provider_config = AppConfig::New();
    cloud_provider_config->url = kCloudProviderFirebaseAppUrl;
    cloud_provider_config->args = fidl::Array<fidl::String>::New(0);
    cloud_provider_app_ = std::make_unique<AppClient<Lifecycle>>(
        user_scope_->GetLauncher(), std::move(cloud_provider_config));
    cloud_provider_app_->services().ConnectToService(
        cloud_provider_factory_.NewRequest());

    cloud_provider = GetCloudProvider();

    // TODO(mesch): Teardown cloud_provider_app_ ?
  }

  ledger_app_->services().ConnectToService(
      ledger_repository_factory_.NewRequest());
  AtEnd(Reset(&ledger_repository_factory_));

  // The directory "/data" is the data root "/data/LEDGER" that the ledger app
  // client is configured to.
  ledger_repository_factory_->GetRepository(
      "/data", std::move(cloud_provider), ledger_repository_.NewRequest(),
      [this](ledger::Status status) {
        if (status != ledger::Status::OK) {
          FXL_LOG(ERROR)
              << "LedgerRepositoryFactory.GetRepository() failed: "
              << LedgerStatusToString(status) << std::endl
              << "CALLING Logout() DUE TO UNRECOVERABLE LEDGER ERROR.";
          Logout();
        }
      });

  // If ledger state is erased from underneath us (happens when the cloud store
  // is cleared), ledger will close the connection to |ledger_repository_|.
  ledger_repository_.set_error_handler([this] { Logout(); });
  AtEnd(Reset(&ledger_repository_));

  ledger_client_.reset(
      new LedgerClient(ledger_repository_.get(), kAppId, [this] {
        FXL_LOG(ERROR) << "CALLING Logout() DUE TO UNRECOVERABLE LEDGER ERROR.";
        Logout();
      }));
  AtEnd(Reset(&ledger_client_));
}

void UserRunnerImpl::InitializeLedgerDashboard() {
  if (test_)
    return;
  ledger_dashboard_scope_ = std::make_unique<Scope>(
      user_scope_->environment(), std::string(kLedgerDashboardEnvLabel));
  AtEnd(Reset(&ledger_dashboard_scope_));

  ledger_dashboard_scope_->AddService<ledger::LedgerRepositoryDebug>(
      [this](fidl::InterfaceRequest<ledger::LedgerRepositoryDebug> request) {
        if (ledger_repository_) {
          ledger_repository_->GetLedgerRepositoryDebug(
              std::move(request), [](ledger::Status status) {
                if (status != ledger::Status::OK) {
                  FXL_LOG(ERROR)
                      << "LedgerRepository.GetLedgerRepositoryDebug() failed: "
                      << LedgerStatusToString(status);
                }
              });
        }
      });

  auto ledger_dashboard_config = AppConfig::New();
  ledger_dashboard_config->url = kLedgerDashboardUrl;

  ledger_dashboard_app_ = std::make_unique<AppClient<Lifecycle>>(
      ledger_dashboard_scope_->GetLauncher(),
      std::move(ledger_dashboard_config));

  AtEnd(Reset(&ledger_dashboard_app_));
  AtEnd(
      Teardown(kBasicTimeout, "LedgerDashboard", ledger_dashboard_app_.get()));

  FXL_LOG(INFO) << "Starting Ledger dashboard " << kLedgerDashboardUrl;
}

void UserRunnerImpl::InitializeDeviceMap() {
  // DeviceMap service
  const std::string device_id = LoadDeviceID(GetAccountId(account_));
  device_name_ = LoadDeviceName(GetAccountId(account_));
  const std::string device_profile = LoadDeviceProfile();

  device_map_impl_ = std::make_unique<DeviceMapImpl>(
      device_name_, device_id, device_profile, ledger_client_.get(),
      fidl::Array<uint8_t>::New(16));
  user_scope_->AddService<DeviceMap>(
      [this](fidl::InterfaceRequest<DeviceMap> request) {
        // device_map_impl_ may be reset before user_scope_.
        if (device_map_impl_) {
          device_map_impl_->Connect(std::move(request));
        }
      });
  AtEnd(Reset(&device_map_impl_));
}

void UserRunnerImpl::InitializeClipboard() {
  agent_runner_->ConnectToAgent(kAppId, kClipboardAgentUrl,
                                services_from_clipboard_agent_.NewRequest(),
                                clipboard_agent_controller_.NewRequest());
  user_scope_->AddService<Clipboard>(
      [this](fidl::InterfaceRequest<Clipboard> request) {
        services_from_clipboard_agent_->ConnectToService(Clipboard::Name_,
                                                         request.TakeChannel());
      });
}

void UserRunnerImpl::InitializeRemoteInvoker() {
  // TODO(planders) Do not create RemoteInvoker until service is actually
  // requested.
  remote_invoker_impl_ =
      std::make_unique<RemoteInvokerImpl>(ledger_client_->ledger());
  user_scope_->AddService<RemoteInvoker>(
      [this](fidl::InterfaceRequest<RemoteInvoker> request) {
        // remote_invoker_impl_ may be reset before user_scope_.
        if (remote_invoker_impl_) {
          remote_invoker_impl_->Connect(std::move(request));
        }
      });
  AtEnd(Reset(&remote_invoker_impl_));
}

void UserRunnerImpl::InitializeMessageQueueManager() {
  std::string message_queue_path = kMessageQueuePath;
  message_queue_path.append(GetAccountId(account_));
  if (!files::CreateDirectory(message_queue_path)) {
    FXL_LOG(FATAL) << "Failed to create message queue directory: "
                   << message_queue_path;
  }

  message_queue_manager_ = std::make_unique<MessageQueueManager>(
      ledger_client_.get(), to_array(kMessageQueuePageId), message_queue_path);
  AtEnd(Reset(&message_queue_manager_));
}

void UserRunnerImpl::InitializeMaxwell(const fidl::String& user_shell_url,
                                       AppConfigPtr story_shell) {
  // NOTE: There is an awkward service exchange here between
  // UserIntelligenceProvider, AgentRunner, StoryProviderImpl,
  // FocusHandler, VisibleStoriesHandler.
  //
  // AgentRunner needs a UserIntelligenceProvider to expose services
  // from Maxwell through its GetIntelligenceServices() method.
  // Initializing the Maxwell process (through
  // UserIntelligenceProviderFactory) requires a ComponentContext.
  // ComponentContext requires an AgentRunner, which creates a
  // circular dependency.
  //
  // Because of FIDL late bindings, we can get around this by creating
  // a new InterfaceRequest here (|intelligence_provider_request|),
  // making the InterfacePtr a valid proxy to be passed to AgentRunner
  // and StoryProviderImpl, even though it won't be bound to a real
  // implementation (provided by Maxwell) until later. It works, but
  // it's not a good pattern.
  //
  // A similar relationship holds between FocusHandler and
  // UserIntelligenceProvider.
  auto intelligence_provider_request = user_intelligence_provider_.NewRequest();
  AtEnd(Reset(&user_intelligence_provider_));

  fidl::InterfaceHandle<maxwell::ContextEngine> context_engine;
  auto context_engine_request = context_engine.NewRequest();

  fidl::InterfaceHandle<StoryProvider> story_provider;
  auto story_provider_request = story_provider.NewRequest();

  fidl::InterfaceHandle<FocusProvider> focus_provider_maxwell;
  auto focus_provider_request_maxwell = focus_provider_maxwell.NewRequest();

  fidl::InterfaceHandle<VisibleStoriesProvider> visible_stories_provider;
  auto visible_stories_provider_request = visible_stories_provider.NewRequest();

  // Start kMaxwellUrl
  auto maxwell_config = AppConfig::New();
  maxwell_config->url = kMaxwellUrl;
  if (test_) {
    maxwell_config->args.push_back(
        "--config=/system/data/maxwell/test_config.json");
  }

  maxwell_app_ =
      std::make_unique<AppClient<maxwell::UserIntelligenceProviderFactory>>(
          user_scope_->GetLauncher(), std::move(maxwell_config));
  maxwell_app_->primary_service()->GetUserIntelligenceProvider(
      std::move(context_engine), std::move(story_provider),
      std::move(focus_provider_maxwell), std::move(visible_stories_provider),
      std::move(intelligence_provider_request));
  AtEnd(Reset(&maxwell_app_));
  AtEnd(Teardown(kBasicTimeout, "Maxwell", maxwell_app_.get()));

  entity_provider_runner_ = std::make_unique<EntityProviderRunner>(
      static_cast<EntityProviderLauncher*>(this));
  AtEnd(Reset(&entity_provider_runner_));

  agent_runner_storage_ = std::make_unique<AgentRunnerStorageImpl>(
      ledger_client_.get(), to_array(kAgentRunnerPageId));
  AtEnd(Reset(&agent_runner_storage_));

  agent_runner_.reset(new AgentRunner(
      user_scope_->GetLauncher(), message_queue_manager_.get(),
      ledger_repository_.get(), agent_runner_storage_.get(),
      token_provider_factory_.get(), user_intelligence_provider_.get(),
      entity_provider_runner_.get()));
  AtEnd(Teardown(kAgentRunnerTimeout, "AgentRunner", &agent_runner_));

  maxwell_component_context_bindings_ = std::make_unique<fidl::BindingSet<
      ComponentContext, std::unique_ptr<ComponentContextImpl>>>();
  AtEnd(Reset(&maxwell_component_context_bindings_));

  ComponentContextInfo component_context_info{
      message_queue_manager_.get(), agent_runner_.get(),
      ledger_repository_.get(), entity_provider_runner_.get()};
  // Start kContextEngineUrl.
  {
    context_engine_ns_services_.AddService<ComponentContext>(
        [this, component_context_info](
            fidl::InterfaceRequest<ComponentContext> request) {
          maxwell_component_context_bindings_->AddBinding(
              std::make_unique<ComponentContextImpl>(
                  component_context_info, kContextEngineComponentNamespace,
                  kContextEngineUrl, kContextEngineUrl),
              std::move(request));
        });
    auto service_list = app::ServiceList::New();
    service_list->names.push_back(ComponentContext::Name_);
    context_engine_ns_services_.AddBinding(service_list->provider.NewRequest());

    auto context_engine_config = AppConfig::New();
    context_engine_config->url = kContextEngineUrl;

    context_engine_app_ = std::make_unique<AppClient<Lifecycle>>(
        user_scope_->GetLauncher(), std::move(context_engine_config),
        "" /* data_origin */, std::move(service_list));
    context_engine_app_->services().ConnectToService(
        std::move(context_engine_request));
    AtEnd(Reset(&context_engine_app_));
    AtEnd(Teardown(kBasicTimeout, "ContextEngine", context_engine_app_.get()));
  }

  auto maxwell_app_component_context =
      maxwell_component_context_bindings_->AddBinding(
          std::make_unique<ComponentContextImpl>(component_context_info,
                                                 kMaxwellComponentNamespace,
                                                 kMaxwellUrl, kMaxwellUrl));
  user_intelligence_provider_->StartAgents(
      std::move(maxwell_app_component_context));

  auto component_scope = maxwell::ComponentScope::New();
  component_scope->set_global_scope(maxwell::GlobalScope::New());
  user_intelligence_provider_->GetComponentIntelligenceServices(
      std::move(component_scope), intelligence_services_.NewRequest());

  user_scope_->AddService<resolver::Resolver>(
      [this](fidl::InterfaceRequest<resolver::Resolver> request) {
        if (user_intelligence_provider_) {
          user_intelligence_provider_->GetResolver(std::move(request));
        }
      });

  // Setup for kModuleResolverUrl
  {
    module_resolver_ns_services_.AddService<maxwell::IntelligenceServices>(
        [this](fidl::InterfaceRequest<maxwell::IntelligenceServices> request) {
          auto component_scope = maxwell::ComponentScope::New();
          component_scope->set_global_scope(maxwell::GlobalScope::New());
          fidl::InterfaceHandle<maxwell::IntelligenceServices>
              intelligence_services;
          if (user_intelligence_provider_) {
            user_intelligence_provider_->GetComponentIntelligenceServices(
                std::move(component_scope), std::move(request));
          }
        });
    module_resolver_ns_services_.AddService<modular::ComponentContext>(
        [this, component_context_info](
            fidl::InterfaceRequest<modular::ComponentContext> request) {
          maxwell_component_context_bindings_->AddBinding(
              std::make_unique<ComponentContextImpl>(
                  component_context_info, kMaxwellComponentNamespace,
                  kModuleResolverUrl, kModuleResolverUrl),
              std::move(request));
        });
    auto service_list = app::ServiceList::New();
    service_list->names.push_back(maxwell::IntelligenceServices::Name_);
    service_list->names.push_back(modular::ComponentContext::Name_);
    module_resolver_ns_services_.AddBinding(
        service_list->provider.NewRequest());

    auto module_resolver_config = AppConfig::New();
    module_resolver_config->url = kModuleResolverUrl;
    // For now, we want data_origin to be "", which uses our (parent process's)
    // /data. This is appropriate for the module_resolver. We can in the future
    // isolate the data it reads to a subdir of /data and map that in here.
    module_resolver_app_ = std::make_unique<AppClient<Lifecycle>>(
        user_scope_->GetLauncher(), std::move(module_resolver_config),
        "" /* data_origin */, std::move(service_list));
    AtEnd(Reset(&module_resolver_app_));
    AtEnd(Teardown(kBasicTimeout, "Resolver", module_resolver_app_.get()));
  }

  module_resolver_app_->services().ConnectToService(
      module_resolver_service_.NewRequest());
  AtEnd(Reset(&module_resolver_service_));
  // End kModuleResolverUrl

  user_shell_component_context_impl_ = std::make_unique<ComponentContextImpl>(
      component_context_info, kUserShellComponentNamespace, user_shell_url,
      user_shell_url);

  AtEnd(Reset(&user_shell_component_context_impl_));

  fidl::InterfacePtr<FocusProvider> focus_provider_story_provider;
  auto focus_provider_request_story_provider =
      focus_provider_story_provider.NewRequest();

  // We create |story_provider_impl_| after |agent_runner_| so
  // story_provider_impl_ is termiated before agent_runner_ because the modules
  // running in a story might freak out if agents they are connected to go away
  // while they are still running. On the other hand agents are meant to outlive
  // story lifetimes.
  story_provider_impl_.reset(new StoryProviderImpl(
      user_scope_.get(), device_map_impl_->current_device_id(),
      ledger_client_.get(), fidl::Array<uint8_t>::New(16),
      std::move(story_shell), component_context_info,
      std::move(focus_provider_story_provider), intelligence_services_.get(),
      user_intelligence_provider_.get(), module_resolver_service_.get(),
      test_));
  story_provider_impl_->Connect(std::move(story_provider_request));

  AtEnd(
      Teardown(kStoryProviderTimeout, "StoryProvider", &story_provider_impl_));

  focus_handler_ = std::make_unique<FocusHandler>(
      device_map_impl_->current_device_id(), ledger_client_.get(),
      fidl::Array<uint8_t>::New(16));
  focus_handler_->AddProviderBinding(std::move(focus_provider_request_maxwell));
  focus_handler_->AddProviderBinding(
      std::move(focus_provider_request_story_provider));

  visible_stories_handler_ = std::make_unique<VisibleStoriesHandler>();
  visible_stories_handler_->AddProviderBinding(
      std::move(visible_stories_provider_request));

  AtEnd(Reset(&focus_handler_));
  AtEnd(Reset(&visible_stories_handler_));
}

void UserRunnerImpl::InitializeUserShell(
    AppConfigPtr user_shell,
    fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request) {
  user_shell_app_ = std::make_unique<AppClient<Lifecycle>>(
      user_scope_->GetLauncher(), std::move(user_shell));
  user_shell_app_->services().ConnectToService(user_shell_.NewRequest());

  AtEnd(Reset(&user_shell_app_));
  AtEnd(Reset(&user_shell_));
  AtEnd(Teardown(kBasicTimeout, "UserShell", user_shell_app_.get()));

  user_shell_app_->SetAppErrorHandler([this] {
    FXL_LOG(ERROR) << "User Shell seems to have crashed unexpectedly."
                   << "Logging out.";
    Logout();
  });

  mozart::ViewProviderPtr view_provider;
  user_shell_app_->services().ConnectToService(view_provider.NewRequest());
  view_provider->CreateView(std::move(view_owner_request), nullptr);

  user_shell_->Initialize(user_shell_context_binding_.NewBinding());
}

void UserRunnerImpl::Terminate(std::function<void()> done) {
  FXL_LOG(INFO) << "UserRunner::Terminate()";
  at_end_done_ = std::move(done);

  TerminateRecurse(at_end_.size() - 1);
}

void UserRunnerImpl::DumpState(const DumpStateCallback& callback) {
  std::ostringstream output;
  output << "=================Begin user info====================" << std::endl;

  output << "=================Begin account info=================" << std::endl;
  std::string account_json;
  XdrWrite(&account_json, &account_, XdrAccount);
  output << account_json << std::endl;

  story_provider_impl_->DumpState(fxl::MakeCopyable(
      [output = std::move(output), callback](const std::string& debug) mutable {
        output << debug;
        callback(output.str());
      }));

  // TODO(alhaad): Add debug info about agents, device map, etc.
}

void UserRunnerImpl::GetAccount(const GetAccountCallback& callback) {
  callback(account_.Clone());
}

void UserRunnerImpl::GetAgentProvider(
    fidl::InterfaceRequest<AgentProvider> request) {
  agent_runner_->Connect(std::move(request));
}

void UserRunnerImpl::GetComponentContext(
    fidl::InterfaceRequest<ComponentContext> request) {
  user_shell_component_context_impl_->Connect(std::move(request));
}

void UserRunnerImpl::GetDeviceName(const GetDeviceNameCallback& callback) {
  callback(device_name_);
}

void UserRunnerImpl::GetFocusController(
    fidl::InterfaceRequest<FocusController> request) {
  focus_handler_->AddControllerBinding(std::move(request));
}

void UserRunnerImpl::GetFocusProvider(
    fidl::InterfaceRequest<FocusProvider> request) {
  focus_handler_->AddProviderBinding(std::move(request));
}

void UserRunnerImpl::GetIntelligenceServices(
    fidl::InterfaceRequest<maxwell::IntelligenceServices> request) {
  auto component_scope = maxwell::ComponentScope::New();
  component_scope->set_global_scope(maxwell::GlobalScope::New());
  user_intelligence_provider_->GetComponentIntelligenceServices(
      std::move(component_scope), std::move(request));
}

void UserRunnerImpl::GetLink(fidl::InterfaceRequest<Link> request) {
  if (user_shell_link_) {
    user_shell_link_->Connect(std::move(request),
                              LinkImpl::ConnectionType::Primary);
    return;
  }

  LinkPathPtr link_path = LinkPath::New();
  link_path->module_path = fidl::Array<fidl::String>::New(0);
  link_path->link_name = kUserShellLinkName;
  user_shell_link_ = std::make_unique<LinkImpl>(ledger_client_.get(),
                                                fidl::Array<uint8_t>::New(16),
                                                std::move(link_path), nullptr);
  user_shell_link_->Connect(std::move(request),
                            LinkImpl::ConnectionType::Secondary);
}

void UserRunnerImpl::GetPresentation(
    fidl::InterfaceRequest<mozart::Presentation> request) {
  user_context_->GetPresentation(std::move(request));
}

void UserRunnerImpl::GetSpeechToText(
    fidl::InterfaceRequest<speech::SpeechToText> request) {
  user_intelligence_provider_->GetSpeechToText(std::move(request));
}

void UserRunnerImpl::GetStoryProvider(
    fidl::InterfaceRequest<StoryProvider> request) {
  story_provider_impl_->Connect(std::move(request));
}

void UserRunnerImpl::GetSuggestionProvider(
    fidl::InterfaceRequest<maxwell::SuggestionProvider> request) {
  user_intelligence_provider_->GetSuggestionProvider(std::move(request));
}

void UserRunnerImpl::GetVisibleStoriesController(
    fidl::InterfaceRequest<VisibleStoriesController> request) {
  visible_stories_handler_->AddControllerBinding(std::move(request));
}

void UserRunnerImpl::Logout() {
  user_context_->Logout();
}

// |EntityProviderLauncher|
void UserRunnerImpl::ConnectToEntityProvider(
    const std::string& agent_url,
    fidl::InterfaceRequest<EntityProvider> entity_provider_request,
    fidl::InterfaceRequest<AgentController> agent_controller_request) {
  FXL_DCHECK(agent_runner_.get());
  agent_runner_->ConnectToEntityProvider(agent_url,
                                         std::move(entity_provider_request),
                                         std::move(agent_controller_request));
}

cloud_provider::CloudProviderPtr UserRunnerImpl::GetCloudProvider() {
  cloud_provider::CloudProviderPtr cloud_provider;
  fidl::InterfaceHandle<auth::TokenProvider> ledger_token_provider;
  token_provider_factory_->GetTokenProvider(kLedgerAppUrl,
                                            ledger_token_provider.NewRequest());
  auto firebase_config = GetLedgerFirebaseConfig();

  cloud_provider_factory_->GetCloudProvider(
      std::move(firebase_config), std::move(ledger_token_provider),
      cloud_provider.NewRequest(), [](cloud_provider::Status status) {
        if (status != cloud_provider::Status::OK) {
          FXL_LOG(ERROR) << "Failed to create a cloud provider: " << status;
        }
      });
  return cloud_provider;
}

void UserRunnerImpl::AtEnd(std::function<void(std::function<void()>)> action) {
  at_end_.emplace_back(std::move(action));
}

void UserRunnerImpl::TerminateRecurse(const int i) {
  if (i >= 0) {
    at_end_[i]([this, i] { TerminateRecurse(i - 1); });
  } else {
    FXL_LOG(INFO) << "UserRunner::Terminate(): done";
    at_end_done_();
  }
}

}  // namespace modular
