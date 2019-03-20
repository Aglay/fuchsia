// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/basemgr/session_provider.h"

#include "peridot/bin/basemgr/session_context_impl.h"
#include "peridot/lib/fidl/clone.h"

namespace modular {

SessionProvider::SessionProvider(
    Delegate* const delegate, fuchsia::sys::Launcher* const launcher,
    const fuchsia::modular::AppConfig& sessionmgr,
    const fuchsia::modular::AppConfig& session_shell,
    const fuchsia::modular::AppConfig& story_shell,
    bool use_session_shell_for_story_shell_factory,
    fit::function<void()> on_zero_sessions)
    : delegate_(delegate),
      launcher_(launcher),
      sessionmgr_(sessionmgr),
      session_shell_(session_shell),
      story_shell_(story_shell),
      use_session_shell_for_story_shell_factory_(
          use_session_shell_for_story_shell_factory),
      on_zero_sessions_(std::move(on_zero_sessions)) {}

void SessionProvider::StartSession(
    fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> view_owner,
    fuchsia::modular::auth::AccountPtr account,
    fuchsia::auth::TokenManagerPtr ledger_token_manager,
    fuchsia::auth::TokenManagerPtr agent_token_manager) {
  if (session_context_) {
    FXL_LOG(WARNING) << "StartSession() called when session context already "
                        "exists. Try calling SessionProvider::Teardown()";
    return;
  }

  auto done = [this](bool logout_users) {
    auto delete_session_context = [this] {
      session_context_.reset();
      on_zero_sessions_();
    };

    if (logout_users) {
      delegate_->LogoutUsers(
          [this, delete_session_context]() { delete_session_context(); });
    } else {
      delete_session_context();
    }
  };

  // Session context initializes and holds the sessionmgr process.
  session_context_ = std::make_unique<SessionContextImpl>(
      launcher_, CloneStruct(sessionmgr_), CloneStruct(session_shell_),
      CloneStruct(story_shell_), use_session_shell_for_story_shell_factory_,
      std::move(ledger_token_manager), std::move(agent_token_manager),
      std::move(account), std::move(view_owner),
      /* get_presentation= */
      [this](
          fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> request) {
        delegate_->GetPresentation(std::move(request));
      },
      done);
}

void SessionProvider::Teardown(fit::function<void()> callback) {
  if (!session_context_) {
    callback();
    return;
  }

  // Shutdown will execute the given |callback|, then destroy
  // |session_context_|. Here we do not logout any users because this is part of
  // teardown (device shutting down, going to sleep, etc.).
  session_context_->Shutdown(/* logout_users= */ false, std::move(callback));
}

FuturePtr<> SessionProvider::SwapSessionShell(
    fuchsia::modular::AppConfig session_shell_config) {
  if (!session_context_) {
    return Future<>::CreateCompleted("SwapSessionShell(Completed)");
  }

  return session_context_->SwapSessionShell(std::move(session_shell_config));
}

void SessionProvider::RestartSession(
    fit::function<void()> on_restart_complete) {
  if (!session_context_) {
    return;
  }

  // Shutting down a session and preserving the users effectively restarts the
  // session.
  session_context_->Shutdown(/* logout_users= */ false,
                             std::move(on_restart_complete));
}

}  // namespace modular
