// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/engine/session_handler.h"

#include <memory>

#include "garnet/lib/ui/scenic/session.h"
#include "lib/ui/scenic/cpp/commands.h"

namespace scenic_impl {
namespace gfx {

SessionHandler::SessionHandler(CommandDispatcherContext dispatcher_context,
                               SessionManager* session_manager,
                               SessionContext session_context,
                               EventReporter* event_reporter,
                               ErrorReporter* error_reporter)
    : TempSessionDelegate(std::move(dispatcher_context)),
      session_manager_(session_manager),
      event_reporter_(event_reporter),
      error_reporter_(error_reporter),
      session_(std::make_unique<Session>(context()->session_id(),
                                         std::move(session_context),
                                         event_reporter, error_reporter)) {
  FXL_DCHECK(session_manager_);
}

SessionHandler::~SessionHandler() { CleanUp(); }

void SessionHandler::CleanUp() {
  if (session_.get() != nullptr) {
    session_manager_->RemoveSessionHandler(session_->id());
    session_.reset();
  }
}

void SessionHandler::Present(
    uint64_t presentation_time, std::vector<zx::event> acquire_fences,
    std::vector<zx::event> release_fences,
    fuchsia::ui::scenic::Session::PresentCallback callback) {
  if (!session_->ScheduleUpdate(
          presentation_time, std::move(buffered_commands_),
          std::move(acquire_fences), std::move(release_fences),
          std::move(callback))) {
    BeginTearDown();
  } else {
    buffered_commands_.clear();
  }
}

void SessionHandler::HitTest(
    uint32_t node_id, fuchsia::ui::gfx::vec3 ray_origin,
    fuchsia::ui::gfx::vec3 ray_direction,
    fuchsia::ui::scenic::Session::HitTestCallback callback) {
  session_->HitTest(node_id, std::move(ray_origin), std::move(ray_direction),
                    std::move(callback));
}

void SessionHandler::HitTestDeviceRay(
    fuchsia::ui::gfx::vec3 ray_origin, fuchsia::ui::gfx::vec3 ray_direction,
    fuchsia::ui::scenic::Session::HitTestDeviceRayCallback callback) {
  session_->HitTestDeviceRay(std::move(ray_origin), std::move(ray_direction),
                             std::move(callback));
}

void SessionHandler::DispatchCommand(fuchsia::ui::scenic::Command command) {
  FXL_DCHECK(command.Which() == fuchsia::ui::scenic::Command::Tag::kGfx);
  buffered_commands_.emplace_back(std::move(command.gfx()));
}

void SessionHandler::BeginTearDown() {
  // Since this is essentially a self destruct
  // call, it's safest not call anything after this
  context()->KillSession();
}

}  // namespace gfx
}  // namespace scenic_impl
