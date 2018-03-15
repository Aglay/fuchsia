// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_ENGINE_SESSION_HANDLER_H_
#define GARNET_LIB_UI_GFX_ENGINE_SESSION_HANDLER_H_

#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/fxl/tasks/task_runner.h"

#include "garnet/lib/ui/gfx/engine/engine.h"
#include "garnet/lib/ui/gfx/engine/session.h"
#include "garnet/lib/ui/scenic/command_dispatcher.h"
#include "garnet/lib/ui/scenic/event_reporter.h"
#include "garnet/lib/ui/scenic/util/error_reporter.h"
#include "lib/ui/scenic/fidl/events.fidl.h"

namespace scenic {
namespace gfx {

class SceneManagerImpl;

// Implements the Session FIDL interface.  For now, does nothing but buffer
// operations from Enqueue() before passing them all to |session_| when
// Commit() is called.  Eventually, this class may do more work if performance
// profiling suggests to.
class SessionHandler : public TempSessionDelegate, public EventReporter {
 public:
  SessionHandler(CommandDispatcherContext context,
                 Engine* engine,
                 SessionId session_id,
                 EventReporter* event_reporter,
                 ErrorReporter* error_reporter);
  virtual ~SessionHandler();

  scenic::gfx::Session* session() const { return session_.get(); }

  // Flushes enqueued session events to the session listener as a batch.
  void SendEvents(::f1dl::Array<ui::EventPtr> events) override;

 protected:
  // |ui::Session|
  void Enqueue(::f1dl::Array<ui::CommandPtr> commands) override;
  void Present(uint64_t presentation_time,
               ::f1dl::Array<zx::event> acquire_fences,
               ::f1dl::Array<zx::event> release_fences,
               const ui::Session::PresentCallback& callback) override;

  void HitTest(uint32_t node_id,
               scenic::vec3Ptr ray_origin,
               scenic::vec3Ptr ray_direction,
               const ui::Session::HitTestCallback& callback) override;

  void HitTestDeviceRay(scenic::vec3Ptr ray_origin,
                        scenic::vec3Ptr ray_direction,
                        const ui::Session::HitTestCallback& clback) override;

  bool ApplyCommand(const ui::CommandPtr& command) override;

 private:
  friend class SessionManager;

  // Called by |binding_| when the connection closes. Must be invoked within
  // the SessionHandler MessageLoop.
  void BeginTearDown();

  // Called only by Engine. Use BeginTearDown() instead when you need to
  // teardown from within SessionHandler.
  void TearDown();

  SessionManager* const session_manager_;

  EventReporter* const event_reporter_;
  ErrorReporter* const error_reporter_;
  scenic::gfx::SessionPtr session_;

  ::f1dl::Array<scenic::OpPtr> buffered_ops_;
};

}  // namespace gfx
}  // namespace scenic

#endif  // GARNET_LIB_UI_GFX_ENGINE_SESSION_HANDLER_H_
