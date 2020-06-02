// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/basemgr/session_provider.h"

#include <fuchsia/hardware/power/statecontrol/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>
#include <zircon/status.h>

#include <src/modular/lib/pseudo_dir/pseudo_dir_utils.h>

#include "src/lib/intl/intl_property_provider_impl/intl_property_provider_impl.h"
#include "src/modular/lib/fidl/clone.h"
#include "src/modular/lib/modular_config/modular_config.h"
#include "src/modular/lib/modular_config/modular_config_constants.h"

namespace modular {

using intl::IntlPropertyProviderImpl;
using ShutDownReason = SessionContextImpl::ShutDownReason;

const int kMaxCrashRecoveryLimit = 3;

const zx::duration kMaxCrashRecoveryDuration = zx::msec(3600 * 1000);  // 1 hour in milliseconds

SessionProvider::SessionProvider(Delegate* const delegate, fuchsia::sys::Launcher* const launcher,
                                 fuchsia::hardware::power::statecontrol::AdminPtr administrator,
                                 fuchsia::modular::AppConfig sessionmgr,
                                 fuchsia::modular::AppConfig session_shell,
                                 fuchsia::modular::AppConfig story_shell,
                                 bool use_session_shell_for_story_shell_factory,
                                 std::unique_ptr<IntlPropertyProviderImpl> intl_property_provider,
                                 fuchsia::modular::session::ModularConfig config,
                                 fit::function<void()> on_zero_sessions)
    : delegate_(delegate),
      launcher_(launcher),
      administrator_(std::move(administrator)),
      sessionmgr_(std::move(sessionmgr)),
      session_shell_(std::move(session_shell)),
      story_shell_(std::move(story_shell)),
      use_session_shell_for_story_shell_factory_(use_session_shell_for_story_shell_factory),
      on_zero_sessions_(std::move(on_zero_sessions)),
      intl_property_provider_(std::move(intl_property_provider)),
      config_(std::move(config)) {
  last_crash_time_ = zx::clock::get_monotonic();
  // Bind `fuchsia.intl.PropertyProvider` to the implementation instance owned
  // by this class.
  sessionmgr_service_dir_.AddEntry(
      fuchsia::intl::PropertyProvider::Name_,
      std::make_unique<vfs::Service>(intl_property_provider_->GetHandler()));
}

bool SessionProvider::StartSession(fuchsia::ui::views::ViewToken view_token,
                                   bool is_ephemeral_account) {
  if (session_context_) {
    FX_LOGS(WARNING) << "StartSession() called when session context already "
                        "exists. Try calling SessionProvider::Teardown()";
    return false;
  }

  // Set up a service directory for serving `fuchsia.intl.PropertyProvider` to
  // the `Sessionmgr`.
  fidl::InterfaceHandle<fuchsia::io::Directory> dir_handle;
  sessionmgr_service_dir_.Serve(fuchsia::io::OPEN_RIGHT_READABLE | fuchsia::io::OPEN_RIGHT_WRITABLE,
                                dir_handle.NewRequest().TakeChannel());
  auto services = fuchsia::sys::ServiceList::New();
  services->names.push_back(fuchsia::intl::PropertyProvider::Name_);
  services->host_directory = dir_handle.TakeChannel();

  auto done = [this](SessionContextImpl::ShutDownReason shutdown_reason) {
    OnSessionShutdown(shutdown_reason);
  };

  // Create a config directory
  // Channel endpoints for hosting an overriden config if using basemgr_launcher.
  zx::channel client;
  FX_CHECK(zx::channel::create(0u, &config_request_, &client) == ZX_OK);

  // Host the config file in a PseudoDir
  auto basemgr = CloneStruct(config_.basemgr_config());
  auto sessionmgr = CloneStruct(config_.sessionmgr_config());
  auto config_str = ModularConfigReader::GetConfigAsString(&basemgr, &sessionmgr);
  config_dir_ =
      modular::MakeFilePathWithContents(modular_config::kStartupConfigFilePath, config_str);
  config_dir_->Serve(fuchsia::io::OPEN_RIGHT_READABLE, std::move(config_request_));

  // Session context initializes and holds the sessionmgr process.
  session_context_ = std::make_unique<SessionContextImpl>(
      launcher_, is_ephemeral_account, CloneStruct(sessionmgr_), CloneStruct(session_shell_),
      CloneStruct(story_shell_), use_session_shell_for_story_shell_factory_, std::move(view_token),
      std::move(services), std::move(client),
      /* get_presentation= */
      [this](fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> request) {
        delegate_->GetPresentation(std::move(request));
      },
      done);

  return true;
}

void SessionProvider::Teardown(fit::function<void()> callback) {
  if (!session_context_) {
    callback();
    return;
  }

  // Shutdown will execute the given |callback|, then destroy
  // |session_context_|.
  session_context_->Shutdown(ShutDownReason::CRASHED, std::move(callback));
}

FuturePtr<> SessionProvider::SwapSessionShell(fuchsia::modular::AppConfig session_shell_config) {
  if (!session_context_) {
    return Future<>::CreateCompleted("SwapSessionShell(Completed)");
  }

  return session_context_->SwapSessionShell(std::move(session_shell_config));
}

void SessionProvider::RestartSession(fit::function<void()> on_restart_complete) {
  if (!session_context_) {
    return;
  }

  // Shutting down a session and preserving the users effectively restarts the
  // session.
  session_context_->Shutdown(ShutDownReason::CRASHED, std::move(on_restart_complete));
}

void SessionProvider::OnSessionShutdown(SessionContextImpl::ShutDownReason shutdown_reason) {
  if (shutdown_reason == SessionContextImpl::ShutDownReason::CRASHED) {
    if (session_crash_recovery_counter_ != 0) {
      zx::duration duration = zx::clock::get_monotonic() - last_crash_time_;
      // If last retry is 1 hour ago, the counter will be reset
      if (duration > kMaxCrashRecoveryDuration) {
        session_crash_recovery_counter_ = 1;
      }
    }

    // Check if max retry limit is reached
    if (session_crash_recovery_counter_ == kMaxCrashRecoveryLimit) {
      FX_LOGS(ERROR) << "Sessionmgr restart limit reached. Considering "
                        "this an unrecoverable failure.";
      administrator_->Reboot(
          fuchsia::hardware::power::statecontrol::RebootReason::SESSION_FAILURE,
          [](fuchsia::hardware::power::statecontrol::Admin_Reboot_Result status) {
            if (status.is_err()) {
              FX_LOGS(ERROR) << "Failed to reboot: " << zx_status_get_string(status.err());
            }
          });
      return;
    }
    session_crash_recovery_counter_ += 1;
    last_crash_time_ = zx::clock::get_monotonic();
  }

  auto delete_session_context = [this] {
    session_context_.reset();
    on_zero_sessions_();
  };

  delete_session_context();
}

}  // namespace modular
