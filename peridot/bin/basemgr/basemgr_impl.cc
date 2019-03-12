// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/basemgr/basemgr_impl.h"

#include <memory>

#include <fuchsia/auth/cpp/fidl.h>
#include <fuchsia/modular/auth/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/async/cpp/future.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/string.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include "peridot/bin/basemgr/basemgr_settings.h"
#include "peridot/bin/basemgr/session_provider.h"
#include "peridot/bin/basemgr/session_shell_settings/session_shell_settings.h"
#include "peridot/bin/basemgr/user_provider_impl.h"
#include "peridot/bin/basemgr/wait_for_minfs.h"
#include "peridot/lib/common/async_holder.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/app_client.h"
#include "peridot/lib/fidl/clone.h"

namespace modular {

namespace {
#ifdef AUTO_LOGIN_TO_GUEST
constexpr bool kAutoLoginToGuest = true;
#else
constexpr bool kAutoLoginToGuest = false;
#endif

// TODO(MF-134): This key is duplicated in
// topaz/lib/settings/lib/device_info.dart. Remove this key once factory reset
// is provided to topaz as a service.
// The key for factory reset toggles.
constexpr char kFactoryResetKey[] = "FactoryReset";

constexpr char kTokenManagerFactoryUrl[] =
    "fuchsia-pkg://fuchsia.com/token_manager_factory#meta/"
    "token_manager_factory.cmx";

}  // namespace

BasemgrImpl::BasemgrImpl(
    const modular::BasemgrSettings& settings,
    const std::vector<SessionShellSettings>& session_shell_settings,
    fuchsia::sys::Launcher* const launcher,
    fuchsia::ui::policy::PresenterPtr presenter,
    fuchsia::devicesettings::DeviceSettingsManagerPtr device_settings_manager,
    std::function<void()> on_shutdown)
    : settings_(settings),
      session_shell_settings_(session_shell_settings),
      launcher_(launcher),
      presenter_(std::move(presenter)),
      device_settings_manager_(std::move(device_settings_manager)),
      on_shutdown_(std::move(on_shutdown)),
      base_shell_context_binding_(this),
      authentication_context_provider_binding_(this),
      session_provider_("SessionProvider") {
  UpdateSessionShellConfig();

  Start();
}

BasemgrImpl::~BasemgrImpl() = default;

void BasemgrImpl::Connect(
    fidl::InterfaceRequest<fuchsia::modular::internal::BasemgrDebug> request) {
  basemgr_bindings_.AddBinding(this, std::move(request));
}

void BasemgrImpl::StartBaseShell() {
  if (base_shell_running_) {
    FXL_DLOG(INFO) << "StartBaseShell() called when already running";

    return;
  }

  base_shell_app_ = std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
      launcher_, CloneStruct(settings_.base_shell));
  base_shell_app_->services().ConnectToService(base_shell_.NewRequest());

  fuchsia::ui::viewsv1::ViewProviderPtr base_shell_view_provider;
  base_shell_app_->services().ConnectToService(
      base_shell_view_provider.NewRequest());

  // We still need to pass a request for root view to base shell since
  // dev_base_shell (which mimics flutter behavior) blocks until it receives
  // the root view request.
  fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> root_view;
  base_shell_view_provider->CreateView(root_view.NewRequest(), nullptr);

  presentation_container_ = std::make_unique<PresentationContainer>(
      presenter_.get(), std::move(root_view),
      /* shell_settings= */ GetActiveSessionShellSettings(),
      /* on_swap_session_shell= */ [this] { SwapSessionShell(); });

  // TODO(alexmin): Remove BaseShellParams.
  fuchsia::modular::BaseShellParams params;
  base_shell_->Initialize(base_shell_context_binding_.NewBinding(),
                          /* base_shell_params= */ std::move(params));

  base_shell_running_ = true;
}

FuturePtr<> BasemgrImpl::StopBaseShell() {
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

FuturePtr<> BasemgrImpl::StopTokenManagerFactoryApp() {
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

void BasemgrImpl::Start() {
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

  // Wait for persistent data to come up.
  if (!settings_.no_minfs) {
    WaitForMinfs();
  }

  // 1. Initialize user provider.
  token_manager_factory_app_.release();
  fuchsia::modular::AppConfig token_manager_config;
  token_manager_config.url = kTokenManagerFactoryUrl;
  token_manager_factory_app_ =
      std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
          launcher_, CloneStruct(token_manager_config));
  token_manager_factory_app_->services().ConnectToService(
      token_manager_factory_.NewRequest());

  user_provider_impl_ = std::make_unique<UserProviderImpl>(
      token_manager_factory_.get(),
      authentication_context_provider_binding_.NewBinding().Bind(),
      /* on_login= */
      [this](fuchsia::modular::auth::AccountPtr account,
             fuchsia::auth::TokenManagerPtr ledger_token_manager,
             fuchsia::auth::TokenManagerPtr agent_token_manager) {
        OnLogin(std::move(account), std::move(ledger_token_manager),
                std::move(agent_token_manager));
      });

  // 2. Initialize session provider.
  session_provider_.reset(new SessionProvider(
      /* delegate= */ this, launcher_, settings_.sessionmgr,
      session_shell_config_, settings_.story_shell,
      settings_.use_session_shell_for_story_shell_factory,
      /* on_zero_sessions= */
      [this] {
        if (settings_.test) {
          // TODO(MI4-1117): Integration tests currently
          // expect base shell to always be running. So, if
          // we're running under a test, DidLogin() will not
          // shut down the base shell after login; thus this
          // method doesn't need to re-start the base shell
          // after a logout.
          return;
        }

        FXL_DLOG(INFO) << "Re-starting due to logout";
        ShowSetupOrLogin();
      }));

  // 3. Show setup UI or proceed to auto-login into a session.
  ShowSetupOrLogin();

  ReportEvent(ModularEvent::BOOTED_TO_BASEMGR);
}

void BasemgrImpl::GetUserProvider(
    fidl::InterfaceRequest<fuchsia::modular::UserProvider> request) {
  user_provider_impl_->Connect(std::move(request));
}

void BasemgrImpl::Shutdown() {
  // Prevent the shutdown sequence from running twice.
  if (state_ == State::SHUTTING_DOWN) {
    return;
  }

  state_ = State::SHUTTING_DOWN;

  FXL_DLOG(INFO) << "fuchsia::modular::BaseShellContext::Shutdown()";

  if (settings_.test) {
    FXL_LOG(INFO)
        << std::endl
        << "============================================================"
        << std::endl
        << "======================== [" << settings_.test_name << "] Done";
  }

  // |session_provider_| teardown is asynchronous because it holds the
  // sessionmgr processes.
  session_provider_.Teardown(kSessionProviderTimeout, [this] {
    StopBaseShell()->Then([this] {
      FXL_DLOG(INFO) << "- fuchsia::modular::BaseShell down";
      user_provider_impl_.reset();
      FXL_DLOG(INFO) << "- fuchsia::modular::UserProvider down";

      StopTokenManagerFactoryApp()->Then([this] {
        FXL_DLOG(INFO) << "- fuchsia::auth::TokenManagerFactory down";
        FXL_LOG(INFO) << "Clean shutdown";
        on_shutdown_();
      });
    });
  });
}

void BasemgrImpl::GetAuthenticationUIContext(
    fidl::InterfaceRequest<fuchsia::auth::AuthenticationUIContext> request) {
  // TODO(MI4-1107): Basemgr needs to implement AuthenticationUIContext
  // itself, and proxy calls for StartOverlay & StopOverlay to BaseShell,
  // starting it if it's not running yet.
  base_shell_->GetAuthenticationUIContext(std::move(request));
}

void BasemgrImpl::OnLogin(fuchsia::modular::auth::AccountPtr account,
                          fuchsia::auth::TokenManagerPtr ledger_token_manager,
                          fuchsia::auth::TokenManagerPtr agent_token_manager) {
  if (session_shell_view_owner_.is_bound()) {
    session_shell_view_owner_.Unbind();
  }
  auto view_owner = session_shell_view_owner_.NewRequest();

  session_provider_->StartSession(std::move(view_owner), std::move(account),
                                  std::move(ledger_token_manager),
                                  std::move(agent_token_manager));

  // TODO(MI4-1117): Integration tests currently expect base shell to always be
  // running. So, if we're running under a test, do not shut down the base shell
  // after login.
  if (!settings_.test) {
    FXL_DLOG(INFO) << "Stopping base shell due to login";
    StopBaseShell();
  }

  // Ownership of the Presenter should be moved to the session shell for tests
  // that enable presenter, and production code.
  if (!settings_.test || settings_.enable_presenter) {
    presentation_container_ = std::make_unique<PresentationContainer>(
        presenter_.get(), std::move(session_shell_view_owner_),
        /* shell_settings= */ GetActiveSessionShellSettings(),
        /* on_swap_session_shell= */ [this] { SwapSessionShell(); });
  }
}

void BasemgrImpl::SwapSessionShell() {
  if (state_ == State::SHUTTING_DOWN) {
    FXL_DLOG(INFO) << "SwapSessionShell() not supported while shutting down";
    return;
  }

  if (session_shell_settings_.empty()) {
    FXL_DLOG(INFO) << "No session shells has been defined";
    return;
  }
  auto shell_count = session_shell_settings_.size();
  if (shell_count <= 1) {
    FXL_DLOG(INFO)
        << "Only one session shell has been defined so switch is disabled";
    return;
  }
  active_session_shell_settings_index_ =
      (active_session_shell_settings_index_ + 1) % shell_count;

  UpdateSessionShellConfig();

  session_provider_->SwapSessionShell(CloneStruct(session_shell_config_))
      ->Then([] { FXL_DLOG(INFO) << "Swapped session shell"; });
}

const SessionShellSettings& BasemgrImpl::GetActiveSessionShellSettings() {
  if (active_session_shell_settings_index_ >= session_shell_settings_.size()) {
    FXL_LOG(ERROR) << "Active session shell index is "
                   << active_session_shell_settings_index_ << ", but only "
                   << session_shell_settings_.size()
                   << " session shell settings exist.";
    return default_session_shell_settings_;
  }

  return session_shell_settings_[active_session_shell_settings_index_];
}

void BasemgrImpl::UpdateSessionShellConfig() {
  // The session shell settings overrides the session_shell flag passed via
  // command line, except in integration tests. TODO(MF-113): Consolidate
  // the session shell settings.
  fuchsia::modular::AppConfig session_shell_config;
  if (settings_.test || session_shell_settings_.empty()) {
    session_shell_config = CloneStruct(settings_.session_shell);
  } else {
    const auto& settings =
        session_shell_settings_[active_session_shell_settings_index_];
    session_shell_config.url = settings.name;
  }

  session_shell_config_ = std::move(session_shell_config);
}

void BasemgrImpl::ShowSetupOrLogin() {
  auto show_setup_or_login = [this] {
    // If there are no session shell settings specified, default to showing
    // setup.
    if (active_session_shell_settings_index_ >=
        session_shell_settings_.size()) {
      StartBaseShell();
      return;
    }

    if (kAutoLoginToGuest) {
      user_provider_impl_->Login(fuchsia::modular::UserLoginParams());
    } else {
      user_provider_impl_->PreviousUsers(
          [this](std::vector<fuchsia::modular::auth::Account> accounts) {
            if (accounts.empty()) {
              StartBaseShell();
            } else {
              fuchsia::modular::UserLoginParams params;
              params.account_id = accounts.at(0).id;
              user_provider_impl_->Login(std::move(params));
            }
          });
    }
  };

  // TODO(MF-134): Improve the factory reset logic by deleting more than just
  // the user data.
  // If the device needs factory reset, remove all the users before proceeding
  // with setup.
  device_settings_manager_.set_error_handler(
      [show_setup_or_login](zx_status_t status) { show_setup_or_login(); });
  device_settings_manager_->GetInteger(
      kFactoryResetKey,
      [this, show_setup_or_login](int factory_reset_value,
                                  fuchsia::devicesettings::Status status) {
        if (status == fuchsia::devicesettings::Status::ok &&
            factory_reset_value > 0) {
          // Unset the factory reset flag.
          device_settings_manager_->SetInteger(
              kFactoryResetKey, 0, [](bool result) {
                if (!result) {
                  FXL_LOG(WARNING) << "Factory reset flag was not updated.";
                }
              });

          user_provider_impl_->RemoveAllUsers([this] { StartBaseShell(); });
        } else {
          show_setup_or_login();
        }
      });
}

void BasemgrImpl::RestartSession(RestartSessionCallback on_restart_complete) {
  session_provider_->RestartSession(on_restart_complete);
}

void BasemgrImpl::LoginAsGuest() {
  fuchsia::modular::UserLoginParams params;
  user_provider_impl_->Login(std::move(params));
}

void BasemgrImpl::LogoutUsers(std::function<void()> callback) {
  user_provider_impl_->RemoveAllUsers(std::move(callback));
}

void BasemgrImpl::GetPresentation(
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> request) {
  presentation_container_->GetPresentation(std::move(request));
}

}  // namespace modular
