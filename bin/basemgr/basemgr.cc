// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include <fs/pseudo-file.h>
#include <fuchsia/auth/cpp/fidl.h>
#include <fuchsia/modular/auth/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/future.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/array.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/string.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>
#include <trace-provider/provider.h>

#include "peridot/bin/basemgr/cobalt/cobalt.h"
#include "peridot/bin/basemgr/user_provider_impl.h"
#include "peridot/lib/common/async_holder.h"
#include "peridot/lib/common/names.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/app_client.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/session_shell_settings/session_shell_settings.h"
#include "peridot/lib/util/filesystem.h"

namespace modular {

namespace {

class Settings {
 public:
  explicit Settings(const fxl::CommandLine& command_line) {
    base_shell.url = command_line.GetOptionValueWithDefault(
        "base_shell", "userpicker_base_shell");
    story_shell.url =
        command_line.GetOptionValueWithDefault("story_shell", "mondrian");
    sessionmgr.url =
        command_line.GetOptionValueWithDefault("sessionmgr", "sessionmgr");
    session_shell.url = command_line.GetOptionValueWithDefault(
        "session_shell", "ermine_session_shell");
    account_provider.url = command_line.GetOptionValueWithDefault(
        "account_provider", "oauth_token_manager");

    disable_statistics = command_line.HasOption("disable_statistics");
    ignore_monitor = command_line.HasOption("ignore_monitor");
    no_minfs = command_line.HasOption("no_minfs");
    test = command_line.HasOption("test");
    enable_presenter = command_line.HasOption("enable_presenter");
    // fuchsia::auth::TokenManager is used if the settings flag
    // |enable_garnet_token_manager| is enabled or if the file
    // |/data/modular/use_garnet_token_manager| exists. The latter form will be
    // useful for QA to test the flow before we turn on for everyone.
    enable_garnet_token_manager =
        command_line.HasOption("enable_garnet_token_manager") ||
        files::IsFile("/data/modular/use_garnet_token_manager");

    ParseShellArgs(
        command_line.GetOptionValueWithDefault("base_shell_args", ""),
        &base_shell.args);

    ParseShellArgs(
        command_line.GetOptionValueWithDefault("story_shell_args", ""),
        &story_shell.args);

    ParseShellArgs(
        command_line.GetOptionValueWithDefault("sessionmgr_args", ""),
        &sessionmgr.args);

    ParseShellArgs(
        command_line.GetOptionValueWithDefault("session_shell_args", ""),
        &session_shell.args);

    if (test) {
      base_shell.args.push_back("--test");
      story_shell.args.push_back("--test");
      sessionmgr.args.push_back("--test");
      session_shell.args.push_back("--test");
      test_name = FindTestName(session_shell.url, session_shell.args);
      disable_statistics = true;
      ignore_monitor = true;
      no_minfs = true;
    }
  }

  static std::string GetUsage() {
    return R"USAGE(basemgr
      --base_shell=BASE_SHELL
      --base_shell_args=SHELL_ARGS
      --session_shell=SESSION_SHELL
      --session_shell_args=SHELL_ARGS
      --story_shell=STORY_SHELL
      --story_shell_args=SHELL_ARGS
      --account_provider=ACCOUNT_PROVIDER
      --disable_statistics
      --ignore_monitor
      --no_minfs
      --test
      --enable_presenter
      --enable_garnet_token_manager
    DEVICE_NAME: Name which session shell uses to identify this device.
    BASE_SHELL:  URL of the base shell to run.
                Defaults to "userpicker_base_shell".
                For integration testing use "dev_base_shell".
    SESSIONMGR: URL of the sessionmgr to run.
                Defaults to "sessionmgr".
    SESSION_SHELL: URL of the session shell to run.
                Defaults to "ermine_session_shell".
                For integration testing use "dev_session_shell".
    STORY_SHELL: URL of the story shell to run.
                Defaults to "mondrian".
                For integration testing use "dev_story_shell".
    SHELL_ARGS: Comma separated list of arguments. Backslash escapes comma.
    ACCOUNT_PROVIDER: URL of the account provider to use.
                Defaults to "oauth_token_manager".
                For integration tests use "dev_token_manager".)USAGE";
  }

  fuchsia::modular::AppConfig base_shell;
  fuchsia::modular::AppConfig story_shell;
  fuchsia::modular::AppConfig sessionmgr;
  fuchsia::modular::AppConfig session_shell;
  fuchsia::modular::AppConfig account_provider;

  std::string test_name;
  bool disable_statistics;
  bool ignore_monitor;
  bool no_minfs;
  bool test;
  bool enable_presenter;
  bool enable_garnet_token_manager;

 private:
  void ParseShellArgs(const std::string& value,
                      fidl::VectorPtr<fidl::StringPtr>* args) {
    bool escape = false;
    std::string arg;
    for (char i : value) {
      if (escape) {
        arg.push_back(i);
        escape = false;
        continue;
      }

      if (i == '\\') {
        escape = true;
        continue;
      }

      if (i == ',') {
        args->push_back(arg);
        arg.clear();
        continue;
      }

      arg.push_back(i);
    }

    if (!arg.empty()) {
      args->push_back(arg);
    }
  }

  // Extract the test name using knowledge of how Modular structures its
  // command lines for testing.
  static std::string FindTestName(
      const fidl::StringPtr& session_shell,
      const fidl::VectorPtr<fidl::StringPtr>& session_shell_args) {
    const std::string kRootModule = "--root_module";
    std::string result = session_shell;

    for (const auto& session_shell_arg : *session_shell_args) {
      const auto& arg = session_shell_arg.get();
      if (arg.substr(0, kRootModule.size()) == kRootModule) {
        result = arg.substr(kRootModule.size());
      }
    }

    const auto index = result.find_last_of('/');
    if (index == std::string::npos) {
      return result;
    }

    return result.substr(index + 1);
  }

  FXL_DISALLOW_COPY_AND_ASSIGN(Settings);
};

}  // namespace

class BasemgrApp : fuchsia::modular::BaseShellContext,
                   fuchsia::auth::AuthenticationContextProvider,
                   fuchsia::modular::auth::AccountProviderContext,
                   fuchsia::ui::policy::KeyboardCaptureListenerHACK,
                   modular::UserProviderImpl::Delegate {
 public:
  explicit BasemgrApp(const Settings& settings,
                      std::shared_ptr<component::StartupContext> const context,
                      std::function<void()> on_shutdown)
      : settings_(settings),
        user_provider_impl_("UserProviderImpl"),
        context_(std::move(context)),
        on_shutdown_(std::move(on_shutdown)),
        base_shell_context_binding_(this),
        account_provider_context_binding_(this),
        authentication_context_provider_binding_(this) {
    if (!context_->has_environment_services()) {
      FXL_LOG(ERROR) << "Failed to receive services from the environment.";
      exit(1);
    }

    // TODO(SCN-595): Presentation is now discoverable, so we don't need
    // kPresentationService anymore.
    service_namespace_.AddService(presentation_state_.bindings.GetHandler(
                                      presentation_state_.presentation.get()),
                                  kPresentationService);

    if (settings.ignore_monitor) {
      Start();
      return;
    }

    context_->ConnectToEnvironmentService(monitor_.NewRequest());

    monitor_.set_error_handler([](zx_status_t status) {
      FXL_LOG(ERROR) << "No basemgr monitor found.";
      exit(1);
    });

    monitor_->GetConnectionCount([this](uint32_t count) {
      if (count != 1) {
        FXL_LOG(ERROR) << "Another basemgr is running."
                       << " Please use that one, or shut it down first.";
        exit(1);
      }

      Start();
    });
  }

 private:
  void InitializePresentation(
      fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> view_owner) {
    if (settings_.test && !settings_.enable_presenter) {
      return;
    }

    auto presentation_request =
        presentation_state_.presentation.is_bound()
            ? presentation_state_.presentation.Unbind().NewRequest()
            : presentation_state_.presentation.NewRequest();

    context_->ConnectToEnvironmentService<fuchsia::ui::policy::Presenter>()
        ->Present(std::move(view_owner), std::move(presentation_request));

    AddGlobalKeyboardShortcuts(presentation_state_.presentation);

    SetShadowTechnique(presentation_state_.shadow_technique);
  }

  void StartBaseShell() {
    if (base_shell_running_) {
      FXL_DLOG(INFO) << "StartBaseShell() called when already running";

      return;
    }

    base_shell_app_ = std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
        context_->launcher().get(), CloneStruct(settings_.base_shell));
    base_shell_app_->services().ConnectToService(base_shell_.NewRequest());

    fuchsia::ui::viewsv1::ViewProviderPtr base_shell_view_provider;
    base_shell_app_->services().ConnectToService(
        base_shell_view_provider.NewRequest());

    // We still need to pass a request for root view to base shell since
    // dev_base_shell (which mimics flutter behavior) blocks until it receives
    // the root view request.
    fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> root_view;
    base_shell_view_provider->CreateView(root_view.NewRequest(), nullptr);

    InitializePresentation(std::move(root_view));

    // Populate parameters and initialize the base shell.
    fuchsia::modular::BaseShellParams params;
    params.presentation = std::move(presentation_state_.presentation);
    base_shell_->Initialize(base_shell_context_binding_.NewBinding(),
                            std::move(params));

    base_shell_running_ = true;
  }

  FuturePtr<> StopBaseShell() {
    if (!base_shell_running_) {
      FXL_DLOG(INFO) << "StopBaseShell() called when already stopped";

      return Future<>::CreateCompleted("StopBaseShell::Completed");
    }

    auto did_stop = Future<>::Create("StopBaseShell");

    base_shell_app_->Teardown(kBasicTimeout, [did_stop, this] {
      FXL_DLOG(INFO) << "- fuchsia::modular::BaseShell down";

      base_shell_running_ = false;
      did_stop->Complete();
    });

    return did_stop;
  }

  FuturePtr<> StopAccountProvider() {
    if (!account_provider_) {
      FXL_DLOG(INFO) << "StopAccountProvider() called when already stopped";

      return Future<>::CreateCompleted("StopAccountProvider::Completed");
    }

    auto did_stop = Future<>::Create("StopAccountProvider");

    account_provider_->Teardown(kBasicTimeout, [did_stop, this] {
      FXL_DLOG(INFO) << "- fuchsia::modular::auth::AccountProvider down";

      account_provider_.release();
      did_stop->Complete();
    });

    return did_stop;
  }

  FuturePtr<> StopTokenManagerFactoryApp() {
    if (!token_manager_factory_app_) {
      FXL_DLOG(INFO)
          << "StopTokenManagerFactoryApp() called when already stopped";

      return Future<>::CreateCompleted("StopTokenManagerFactoryApp::Completed");
    }

    auto did_stop = Future<>::Create("StopTokenManagerFactoryApp");

    token_manager_factory_app_->Teardown(kBasicTimeout, [did_stop, this] {
      FXL_DLOG(INFO) << "- fuchsia::auth::TokenManagerFactory down";

      token_manager_factory_app_.release();
      did_stop->Complete();
    });

    return did_stop;
  }

  void Start() {
    if (settings_.test) {
      // 0. Print test banner.
      FXL_LOG(INFO)
          << std::endl
          << std::endl
          << "======================== Starting Test [" << settings_.test_name
          << "]" << std::endl
          << "============================================================"
          << std::endl;
    }

    // Start the base shell. This is done first so that we can show some UI
    // until other things come up.
    StartBaseShell();

    // Wait for persistent data to come up.
    if (!settings_.no_minfs) {
      WaitForMinfs();
    }

    // Start OAuth Token Manager App.
    fuchsia::modular::AppConfig token_manager_config;
    if (settings_.enable_garnet_token_manager) {
      token_manager_config.url = "token_manager_factory";
      FXL_DLOG(INFO) << "Initialzing token_manager_factory_app()";
      token_manager_factory_app_ =
          std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
              context_->launcher().get(), CloneStruct(token_manager_config));
      token_manager_factory_app_->services().ConnectToService(
          token_manager_factory_.NewRequest());
    } else {
      token_manager_config.url = settings_.account_provider.url;
      token_manager_factory_app_.release();
    }

    account_provider_ =
        std::make_unique<AppClient<fuchsia::modular::auth::AccountProvider>>(
            context_->launcher().get(), std::move(token_manager_config),
            "/data/modular/ACCOUNT_MANAGER");
    account_provider_->SetAppErrorHandler(
        [] { FXL_CHECK(false) << "Token manager crashed. Stopping basemgr."; });
    account_provider_->primary_service()->Initialize(
        account_provider_context_binding_.NewBinding());

    user_provider_impl_.reset(new UserProviderImpl(
        context_, settings_.sessionmgr, settings_.session_shell,
        settings_.story_shell, account_provider_->primary_service().get(),
        token_manager_factory_.get(),
        authentication_context_provider_binding_.NewBinding().Bind(),
        settings_.enable_garnet_token_manager, this));

    ReportEvent(ModularEvent::BOOTED_TO_BASEMGR);
  }

  // |fuchsia::modular::BaseShellContext|
  void GetUserProvider(
      fidl::InterfaceRequest<fuchsia::modular::UserProvider> request) override {
    user_provider_impl_->Connect(std::move(request));
  }

  // |fuchsia::modular::BaseShellContext|
  void Shutdown() override {
    // TODO(mesch): Some of these could be done in parallel too.
    // fuchsia::modular::UserProvider must go first, but the order after user
    // provider is for now rather arbitrary. We terminate base shell last so
    // that in tests testing::Teardown() is invoked at the latest possible time.
    // Right now it just demonstrates that AppTerminate() works as we like it
    // to.
    FXL_DLOG(INFO) << "fuchsia::modular::BaseShellContext::Shutdown()";

    if (settings_.test) {
      FXL_LOG(INFO)
          << std::endl
          << "============================================================"
          << std::endl
          << "======================== [" << settings_.test_name << "] Done";
    }

    user_provider_impl_.Teardown(kUserProviderTimeout, [this] {
      FXL_DLOG(INFO) << "- fuchsia::modular::UserProvider down";
      StopAccountProvider()->Then([this] {
        FXL_DLOG(INFO) << "- fuchsia::modular::auth::AccountProvider down";
        StopTokenManagerFactoryApp()->Then([this] {
          FXL_DLOG(INFO) << "- fuchsia::auth::TokenManagerFactory down";
          StopBaseShell()->Then([this] {
            FXL_LOG(INFO) << "Clean Shutdown";
            on_shutdown_();
          });
        });
      });
    });
  }

  // |AccountProviderContext|
  void GetAuthenticationContext(
      fidl::StringPtr account_id,
      fidl::InterfaceRequest<fuchsia::modular::auth::AuthenticationContext>
          request) override {
    // TODO(MI4-1107): Basemgr needs to implement AuthenticationContext
    // itself, and proxy calls for StartOverlay & StopOverlay to BaseShell,
    // starting it if it's not running yet.
    FXL_CHECK(base_shell_);
    base_shell_->GetAuthenticationContext(account_id, std::move(request));
  }

  // |AuthenticationContextProvider|
  void GetAuthenticationUIContext(
      fidl::InterfaceRequest<fuchsia::auth::AuthenticationUIContext> request)
      override {
    // TODO(MI4-1107): Basemgr needs to implement AuthenticationUIContext
    // itself, and proxy calls for StartOverlay & StopOverlay to BaseShell,
    // starting it if it's not running yet.
    FXL_CHECK(base_shell_);
    base_shell_->GetAuthenticationUIContext(std::move(request));
  }

  // |UserProviderImpl::Delegate|
  void DidLogin() override {
    // Continues if `enable_presenter` is set to true during testing, as
    // ownership of the Presenter should still be moved to the session shell.
    if (settings_.test && !settings_.enable_presenter) {
      // TODO(MI4-1117): Integration tests currently expect base shell to
      // always be running. So, if we're running under a test, do not shut down
      // the base shell after login.
      return;
    }

    // TODO(MI4-1117): See above. The base shell shouldn't be shut down.
    if (!settings_.test) {
      FXL_DLOG(INFO) << "Stopping base shell due to login";
      StopBaseShell();
    }

    InitializePresentation(session_shell_view_owner_);

    const auto& settings_vector = SessionShellSettings::GetSystemSettings();
    if (active_session_shell_index_ >= settings_vector.size()) {
      FXL_LOG(ERROR) << "Active session shell index is "
                     << active_session_shell_index_ << ", but only "
                     << settings_vector.size() << " session shells exist.";
      return;
    }

    UpdatePresentation(settings_vector[active_session_shell_index_]);
  }

  // |UserProviderImpl::Delegate|
  void DidLogout() override {
    if (settings_.test) {
      // TODO(MI4-1117): Integration tests currently expect base shell to
      // always be running. So, if we're running under a test, DidLogin() will
      // not shut down the base shell after login; thus this method doesn't
      // need to re-start the base shell after a logout.
      return;
    }

    FXL_DLOG(INFO) << "Re-starting base shell due to logout";

    StartBaseShell();
  }

  // |UserProviderImpl::Delegate|
  fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
  GetSessionShellViewOwner(
      fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>) override {
    return session_shell_view_owner_.is_bound()
               ? session_shell_view_owner_.Unbind().NewRequest()
               : session_shell_view_owner_.NewRequest();
  }

  // |UserProviderImpl::Delegate|
  fidl::InterfaceHandle<fuchsia::sys::ServiceProvider>
  GetSessionShellServiceProvider(
      fidl::InterfaceHandle<fuchsia::sys::ServiceProvider>) override {
    fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> handle;
    service_namespace_.AddBinding(handle.NewRequest());
    return handle;
  }

  // |KeyboardCaptureListenerHACK|
  void OnEvent(fuchsia::ui::input::KeyboardEvent event) override {
    switch (event.code_point) {
      case ' ': {
        SwapSessionShell();
        break;
      }
      case 's': {
        SetNextShadowTechnique();
        break;
      }
      case 'l':
        ToggleClipping();
        break;
      default:
        FXL_DLOG(INFO) << "Unknown keyboard event: codepoint="
                       << event.code_point << ", modifiers=" << event.modifiers;
        break;
    }
  }

  void AddGlobalKeyboardShortcuts(
      fuchsia::ui::policy::PresentationPtr& presentation) {
    presentation->CaptureKeyboardEventHACK(
        {
            .code_point = ' ',  // spacebar
            .modifiers = fuchsia::ui::input::kModifierLeftControl,
        },
        keyboard_capture_listener_bindings_.AddBinding(this));
    presentation->CaptureKeyboardEventHACK(
        {
            .code_point = 's',
            .modifiers = fuchsia::ui::input::kModifierLeftControl,
        },
        keyboard_capture_listener_bindings_.AddBinding(this));
    presentation->CaptureKeyboardEventHACK(
        {
            .code_point = 'l',
            .modifiers = fuchsia::ui::input::kModifierRightAlt,
        },
        keyboard_capture_listener_bindings_.AddBinding(this));
  }

  void UpdatePresentation(const SessionShellSettings& settings) {
    if (settings.display_usage != fuchsia::ui::policy::DisplayUsage::kUnknown) {
      FXL_DLOG(INFO) << "Setting display usage: "
                     << fidl::ToUnderlying(settings.display_usage);
      presentation_state_.presentation->SetDisplayUsage(settings.display_usage);
    }

    if (!std::isnan(settings.screen_width) &&
        !std::isnan(settings.screen_height)) {
      FXL_DLOG(INFO) << "Setting display size: " << settings.screen_width
                     << " x " << settings.screen_height;
      presentation_state_.presentation->SetDisplaySizeInMm(
          settings.screen_width, settings.screen_height);
    }
  }

  void SwapSessionShell() {
    if (SessionShellSettings::GetSystemSettings().empty()) {
      FXL_DLOG(INFO) << "No session shells has been defined";
      return;
    }

    active_session_shell_index_ =
        (active_session_shell_index_ + 1) %
        SessionShellSettings::GetSystemSettings().size();
    const auto& settings = SessionShellSettings::GetSystemSettings().at(
        active_session_shell_index_);

    auto session_shell_config = fuchsia::modular::AppConfig::New();
    session_shell_config->url = settings.name;

    user_provider_impl_->SwapSessionShell(std::move(*session_shell_config))
        ->Then([] { FXL_DLOG(INFO) << "Swapped session shell"; });
  }

  void SetNextShadowTechnique() {
    using ShadowTechnique = fuchsia::ui::gfx::ShadowTechnique;

    auto next_shadow_technique =
        [](ShadowTechnique shadow_technique) -> ShadowTechnique {
      switch (shadow_technique) {
        case ShadowTechnique::UNSHADOWED:
          return ShadowTechnique::SCREEN_SPACE;
        case ShadowTechnique::SCREEN_SPACE:
          return ShadowTechnique::SHADOW_MAP;
        default:
          FXL_LOG(ERROR) << "Unknown shadow technique: "
                         << fidl::ToUnderlying(shadow_technique);
          // Fallthrough
        case ShadowTechnique::SHADOW_MAP:
        case ShadowTechnique::MOMENT_SHADOW_MAP:
          return ShadowTechnique::UNSHADOWED;
      }
    };

    SetShadowTechnique(
        next_shadow_technique(presentation_state_.shadow_technique));
  }

  void SetShadowTechnique(fuchsia::ui::gfx::ShadowTechnique shadow_technique) {
    if (!presentation_state_.presentation)
      return;

    presentation_state_.shadow_technique = shadow_technique;

    FXL_LOG(INFO) << "Setting shadow technique to "
                  << fidl::ToUnderlying(presentation_state_.shadow_technique);

    fuchsia::ui::gfx::RendererParam param;
    param.set_shadow_technique(presentation_state_.shadow_technique);

    auto renderer_params =
        fidl::VectorPtr<fuchsia::ui::gfx::RendererParam>::New(0);
    renderer_params.push_back(std::move(param));

    presentation_state_.presentation->SetRendererParams(
        std::move(renderer_params));
  }

  void ToggleClipping() {
    if (!presentation_state_.presentation)
      return;

    FXL_DLOG(INFO) << "Toggling clipping";

    presentation_state_.clipping_enabled =
        !presentation_state_.clipping_enabled;
    presentation_state_.presentation->EnableClipping(
        presentation_state_.clipping_enabled);
  }

  const Settings& settings_;  // Not owned nor copied.

  AsyncHolder<UserProviderImpl> user_provider_impl_;

  std::shared_ptr<component::StartupContext> const context_;
  fuchsia::modular::BasemgrMonitorPtr monitor_;
  std::function<void()> on_shutdown_;

  fidl::Binding<fuchsia::modular::BaseShellContext> base_shell_context_binding_;
  fidl::Binding<fuchsia::modular::auth::AccountProviderContext>
      account_provider_context_binding_;
  fidl::Binding<fuchsia::auth::AuthenticationContextProvider>
      authentication_context_provider_binding_;

  std::unique_ptr<AppClient<fuchsia::modular::auth::AccountProvider>>
      account_provider_;
  std::unique_ptr<AppClient<fuchsia::modular::Lifecycle>>
      token_manager_factory_app_;
  fuchsia::auth::TokenManagerFactoryPtr token_manager_factory_;

  bool base_shell_running_{};
  std::unique_ptr<AppClient<fuchsia::modular::Lifecycle>> base_shell_app_;
  fuchsia::modular::BaseShellPtr base_shell_;

  fidl::BindingSet<fuchsia::ui::policy::KeyboardCaptureListenerHACK>
      keyboard_capture_listener_bindings_;

  fuchsia::ui::viewsv1token::ViewOwnerPtr session_shell_view_owner_;

  struct {
    fuchsia::ui::policy::PresentationPtr presentation;
    fidl::BindingSet<fuchsia::ui::policy::Presentation> bindings;

    fuchsia::ui::gfx::ShadowTechnique shadow_technique =
        fuchsia::ui::gfx::ShadowTechnique::UNSHADOWED;
    bool clipping_enabled{};
  } presentation_state_;

  component::ServiceNamespace service_namespace_;

  std::vector<SessionShellSettings>::size_type active_session_shell_index_{};

  FXL_DISALLOW_COPY_AND_ASSIGN(BasemgrApp);
};

fit::deferred_action<fit::closure> SetupCobalt(
    Settings& settings, async_dispatcher_t* dispatcher,
    component::StartupContext* context) {
  if (settings.disable_statistics) {
    return fit::defer<fit::closure>([] {});
  }
  return InitializeCobalt(dispatcher, context);
};

}  // namespace modular

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (command_line.HasOption("help")) {
    std::cout << modular::Settings::GetUsage() << std::endl;
    return 0;
  }

  modular::Settings settings(command_line);
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  trace::TraceProvider trace_provider(loop.dispatcher());
  auto context = std::shared_ptr<component::StartupContext>(
      component::StartupContext::CreateFromStartupInfo());
  fit::deferred_action<fit::closure> cobalt_cleanup = modular::SetupCobalt(
      settings, std::move(loop.dispatcher()), context.get());

  modular::BasemgrApp app(settings, context, [&loop, &cobalt_cleanup] {
    cobalt_cleanup.call();
    loop.Quit();
  });
  loop.Run();

  return 0;
}
