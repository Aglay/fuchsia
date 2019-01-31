// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/testing/session_shell_impl.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

namespace modular {
namespace testing {

SessionShellImpl::SessionShellImpl() = default;
SessionShellImpl::~SessionShellImpl() = default;

fidl::InterfaceRequestHandler<fuchsia::modular::SessionShell> SessionShellImpl::GetHandler() {
  return bindings_.GetHandler(this);
}

// |SessionShell|
void SessionShellImpl::AttachView(fuchsia::modular::ViewIdentifier view_id,
                                  fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner>
                                  view_owner) {
  on_attach_view_(std::move(view_id));
}

// |SessionShell|
void SessionShellImpl::DetachView(fuchsia::modular::ViewIdentifier view_id,
                                  std::function<void()> done) {
  on_detach_view_(std::move(view_id));

  // Used to simulate a sluggish shell that hits the timeout.
  async::PostDelayedTask(async_get_default_dispatcher(),
                         std::move(done), detach_delay_);
}

}  // namespace testing
}  // namespace modular
