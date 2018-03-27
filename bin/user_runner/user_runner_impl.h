// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_USER_RUNNER_USER_RUNNER_IMPL_H_
#define PERIDOT_BIN_USER_RUNNER_USER_RUNNER_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/presentation.h>
#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/service_provider_impl.h"
#include "lib/auth/fidl/account/account.fidl.h"
#include <fuchsia/cpp/cloud_provider.h>
#include "lib/config/fidl/config.fidl.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/interface_ptr.h"
#include "lib/fxl/macros.h"
#include <fuchsia/cpp/ledger.h>
#include "lib/lifecycle/fidl/lifecycle.fidl.h"
#include "lib/module_resolver/fidl/module_resolver.fidl.h"
#include "lib/resolver/fidl/resolver.fidl.h"
#include "lib/speech/fidl/speech_to_text.fidl.h"
#include "lib/story/fidl/story_provider.fidl.h"
#include "lib/suggestion/fidl/suggestion_provider.fidl.h"
#include "lib/ui/input/fidl/input_events.fidl.h"
#include "lib/user/fidl/user_runner.fidl.h"
#include "lib/user/fidl/user_shell.fidl.h"
#include "lib/user_intelligence/fidl/user_intelligence_provider.fidl.h"
#include "peridot/bin/agent_runner/agent_runner_storage_impl.h"
#include "peridot/bin/cloud_provider_firebase/fidl/factory.fidl.h"
#include "peridot/bin/entity/entity_provider_launcher.h"
#include "peridot/bin/entity/entity_provider_runner.h"
#include "peridot/lib/common/async_holder.h"
#include "peridot/lib/fidl/app_client.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/scope.h"
#include "peridot/lib/fidl/view_host.h"
#include "peridot/lib/rapidjson/rapidjson.h"

namespace modular {

class AgentRunner;
class ComponentContextImpl;
class DeviceMapImpl;
class FocusHandler;
class LedgerClient;
class LinkImpl;
class MessageQueueManager;
class RemoteInvokerImpl;
class StoryProviderImpl;
class VisibleStoriesHandler;

class UserRunnerImpl : UserRunner,
                       UserShellContext,
                       EntityProviderLauncher,
                       UserRunnerDebug {
 public:
  UserRunnerImpl(component::ApplicationContext* application_context, bool test);

  ~UserRunnerImpl() override;

  // |AppDriver| calls this.
  void Terminate(std::function<void()> done);

 private:
  class SwapUserShellOperation;

  // |UserRunner|
  void Initialize(
      auth::AccountPtr account,
      AppConfigPtr user_shell,
      AppConfigPtr story_shell,
      f1dl::InterfaceHandle<auth::TokenProviderFactory> token_provider_factory,
      f1dl::InterfaceHandle<UserContext> user_context,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request) override;

  // |UserRunner|
  void SwapUserShell(AppConfigPtr user_shell,
                     const SwapUserShellCallback& callback) override;

  // Sequence of Initialize() broken up into steps for clarity.
  void InitializeUser(
      auth::AccountPtr account,
      f1dl::InterfaceHandle<auth::TokenProviderFactory> token_provider_factory,
      f1dl::InterfaceHandle<UserContext> user_context);
  void InitializeLedger();
  void InitializeLedgerDashboard();
  void InitializeDeviceMap();
  void InitializeClipboard();
  void InitializeRemoteInvoker();
  void InitializeMessageQueueManager();
  void InitializeMaxwell(const f1dl::StringPtr& user_shell_url,
                         AppConfigPtr story_shell);
  void InitializeUserShell(
      AppConfigPtr user_shell,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request);

  void RunUserShell(AppConfigPtr user_shell);
  // This is a termination sequence that may be used with |AtEnd()|, but also
  // may be executed to terminate the currently running user shell.
  void TerminateUserShell(const std::function<void()>& done);

  // |UserShellContext|
  void GetAccount(const GetAccountCallback& callback) override;
  void GetAgentProvider(f1dl::InterfaceRequest<AgentProvider> request) override;
  void GetComponentContext(
      f1dl::InterfaceRequest<ComponentContext> request) override;
  void GetDeviceName(const GetDeviceNameCallback& callback) override;
  void GetFocusController(
      f1dl::InterfaceRequest<FocusController> request) override;
  void GetFocusProvider(f1dl::InterfaceRequest<FocusProvider> request) override;
  void GetIntelligenceServices(
      f1dl::InterfaceRequest<maxwell::IntelligenceServices> request) override;
  void GetLink(f1dl::InterfaceRequest<Link> request) override;
  void GetPresentation(
      f1dl::InterfaceRequest<presentation::Presentation> request) override;
  void GetSpeechToText(
      f1dl::InterfaceRequest<speech::SpeechToText> request) override;
  void GetStoryProvider(f1dl::InterfaceRequest<StoryProvider> request) override;
  void GetSuggestionProvider(
      f1dl::InterfaceRequest<maxwell::SuggestionProvider> request) override;
  void GetVisibleStoriesController(
      f1dl::InterfaceRequest<VisibleStoriesController> request) override;
  void Logout() override;

  // |EntityProviderLauncher|
  void ConnectToEntityProvider(
      const std::string& component_id,
      f1dl::InterfaceRequest<EntityProvider> entity_provider_request,
      f1dl::InterfaceRequest<AgentController> agent_controller_request)
      override;

  // |UserRunnerDebug|
  void DumpState(const DumpStateCallback& callback) override;

  component::ServiceProviderPtr GetServiceProvider(AppConfigPtr config);
  component::ServiceProviderPtr GetServiceProvider(const std::string& url);

  cloud_provider::CloudProviderPtr GetCloudProvider();

  // Called during initialization. Schedules the given action to be executed
  // during termination. This allows to create something like an asynchronous
  // destructor at initialization time. The sequence of actions thus scheduled
  // is executed in reverse in Terminate().
  //
  // The AtEnd() calls for a field should happen next to the calls that
  // initialize the field, for the following reasons:
  //
  // 1. It ensures the termination sequence corresponds to the initialization
  //    sequence.
  //
  // 2. It is easy to audit that there is a termination action for every
  //    initialization that needs one.
  //
  // 3. Conditional initialization also omits the termination (such as for
  //    agents that are not started when runnign a test).
  //
  // See also the Reset() and Teardown() functions in the .cc file.
  void AtEnd(std::function<void(std::function<void()>)> action);

  // Recursively execute the termiation steps scheduled by AtEnd(). The
  // execution steps are stored in at_end_.
  void TerminateRecurse(int i);

  component::ApplicationContext* const application_context_;
  const bool test_;

  f1dl::BindingSet<UserRunner> bindings_;
  f1dl::BindingSet<UserRunnerDebug> user_runner_debug_bindings_;
  f1dl::Binding<UserShellContext> user_shell_context_binding_;

  auth::TokenProviderFactoryPtr token_provider_factory_;
  UserContextPtr user_context_;
  std::unique_ptr<AppClient<Lifecycle>> cloud_provider_app_;
  cloud_provider_firebase::FactoryPtr cloud_provider_factory_;
  std::unique_ptr<AppClient<ledger::LedgerController>> ledger_app_;
  ledger::LedgerRepositoryFactoryPtr ledger_repository_factory_;
  ledger::LedgerRepositoryPtr ledger_repository_;
  std::unique_ptr<LedgerClient> ledger_client_;
  // Provides services to the Ledger
  component::ServiceProviderImpl ledger_service_provider_;

  std::unique_ptr<Scope> user_scope_;

  auth::AccountPtr account_;

  std::unique_ptr<AppClient<maxwell::UserIntelligenceProviderFactory>>
      maxwell_app_;
  std::unique_ptr<AppClient<Lifecycle>> context_engine_app_;
  std::unique_ptr<AppClient<Lifecycle>> module_resolver_app_;
  std::unique_ptr<AppClient<Lifecycle>> user_shell_app_;
  UserShellPtr user_shell_;
  std::unique_ptr<ViewHost> user_shell_view_host_;

  std::unique_ptr<EntityProviderRunner> entity_provider_runner_;
  AsyncHolder<StoryProviderImpl> story_provider_impl_;
  std::unique_ptr<MessageQueueManager> message_queue_manager_;
  std::unique_ptr<AgentRunnerStorage> agent_runner_storage_;
  AsyncHolder<AgentRunner> agent_runner_;
  std::unique_ptr<DeviceMapImpl> device_map_impl_;
  std::unique_ptr<RemoteInvokerImpl> remote_invoker_impl_;
  std::string device_name_;

  // Services we provide to |context_engine_app_|.
  component::ServiceProviderImpl context_engine_ns_services_;

  // These component contexts are supplied to:
  // - the user intelligence provider (from |maxwell_app_|) so it can run agents
  //   and create message queues
  // - |context_engine_app_| so it can resolve entity references
  // - |modular resolver_service_| so it can resolve entity references
  std::unique_ptr<
      f1dl::BindingSet<ComponentContext, std::unique_ptr<ComponentContextImpl>>>
      maxwell_component_context_bindings_;

  // Service provider interfaces for maxwell services. They are created with
  // the component context above as parameters.
  f1dl::InterfacePtr<maxwell::UserIntelligenceProvider>
      user_intelligence_provider_;
  f1dl::InterfacePtr<maxwell::IntelligenceServices> intelligence_services_;

  // Services we provide to the module resolver's namespace.
  component::ServiceProviderImpl module_resolver_ns_services_;
  ModuleResolverPtr module_resolver_service_;

  std::unique_ptr<FocusHandler> focus_handler_;
  std::unique_ptr<VisibleStoriesHandler> visible_stories_handler_;

  // Component context given to user shell so that it can run agents and
  // create message queues.
  std::unique_ptr<ComponentContextImpl> user_shell_component_context_impl_;

  // Given to the user shell so it can store its own data. These data are
  // shared between all user shells (so it's not private to the user shell
  // *app*).
  std::unique_ptr<LinkImpl> user_shell_link_;

  // For the Ledger Debug Dashboard
  std::unique_ptr<Scope> ledger_dashboard_scope_;
  std::unique_ptr<AppClient<Lifecycle>> ledger_dashboard_app_;

  // Holds the actions scheduled by calls to the AtEnd() method.
  std::vector<std::function<void(std::function<void()>)>> at_end_;

  // Holds the done callback of Terminate() while the at_end_ actions are being
  // executed. We can rely on Terminate() only being called once. (And if not,
  // this could simply be made a vector as usual.)
  std::function<void()> at_end_done_;

  // The service provider used to connect to services advertised by the
  // clipboard agent.
  component::ServiceProviderPtr services_from_clipboard_agent_;

  // The agent controller used to control the clipboard agent.
  AgentControllerPtr clipboard_agent_controller_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UserRunnerImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_USER_RUNNER_USER_RUNNER_IMPL_H_
