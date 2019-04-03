// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_ENGINE_FRAME_SCHEDULER_H_
#define GARNET_LIB_UI_GFX_ENGINE_FRAME_SCHEDULER_H_

#include <unordered_set>

#include "garnet/lib/ui/gfx/id.h"

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/zx/time.h>
#include <src/lib/fxl/memory/weak_ptr.h>

namespace scenic_impl {
namespace gfx {

class FrameTimings;
using FrameTimingsPtr = fxl::RefPtr<FrameTimings>;
using PresentationTime = zx_time_t;

// Interface for performing session updates.
class SessionUpdater {
 public:
  struct SessionUpdate {
    SessionId session_id;
    PresentationTime requested_presentation_time;

    bool operator>(const SessionUpdate& rhs) const {
      return requested_presentation_time > rhs.requested_presentation_time;
    }
  };

  struct UpdateResults {
    bool needs_render = false;
    std::unordered_set<SessionId> sessions_to_reschedule;
  };

  virtual ~SessionUpdater() = default;

  // Applies all updates scheduled before or at |presentation_time|, for each
  // session in |sessions_to_update|. Returns true if any updates were applied,
  // false otherwise.
  virtual UpdateResults UpdateSessions(
      std::unordered_set<SessionId> sessions_to_update,
      zx_time_t presentation_time) = 0;

  // Signals the start of a new frame.
  virtual void NewFrame() = 0;

  // Signal that all updates before the current frame have been presented. The
  // signaled callbacks are every successful present between the last time
  // SignalSuccessfulPresentCallbacks was called and the most recent call to
  // NewFrame().
  virtual void SignalSuccessfulPresentCallbacks(
      fuchsia::images::PresentationInfo) = 0;
};

// Interface for rendering frames.
class FrameRenderer {
 public:
  virtual ~FrameRenderer() = default;

  // Called when it's time to render a new frame.  The FrameTimings object is
  // used to accumulate timing for all swapchains that are used as render
  // targets in that frame.
  //
  // If RenderFrame() returns true, the delegate is responsible for calling
  // FrameTimings::OnFrameRendered/Presented/Dropped(). Otherwise, rendering
  // did not occur for some reason, and the FrameScheduler should not expect to
  // receive any timing information for that frame.
  // TODO(SCN-1089): these return value semantics are not ideal.  See comments
  // in Engine::RenderFrame() regarding this same issue.
  virtual bool RenderFrame(const FrameTimingsPtr& frame_timings,
                           zx_time_t presentation_time) = 0;
};

struct FrameSchedulerDelegate {
  fxl::WeakPtr<FrameRenderer> frame_renderer;
  fxl::WeakPtr<SessionUpdater> session_updater;
};

// The FrameScheduler is responsible for scheduling frames to be drawn in
// response to requests from clients.  When a frame is requested, the
// FrameScheduler will decide at which Vsync the frame should be displayed at.
// This time will be no earlier than the requested time, and will be as close
// as possible to the requested time, subject to various constraints.  For
// example, if the requested time is earlier than the time that rendering would
// finish, were it started immediately, then the frame will be scheduled for a
// later Vsync.
class FrameScheduler {
 public:
  virtual ~FrameScheduler() = default;

  virtual void SetDelegate(FrameSchedulerDelegate delegate) = 0;

  // If |render_continuously|, we keep scheduling new frames immediately after
  // each presented frame, regardless of whether they're explicitly requested
  // using RequestFrame().
  virtual void SetRenderContinuously(bool render_continuously) = 0;

  // Tell the FrameScheduler to schedule a frame. This is also used for
  // updates triggered by something other than a Session update i.e. an
  // ImagePipe with a new Image to present.
  virtual void ScheduleUpdateForSession(zx_time_t presentation_time,
                                        scenic_impl::SessionId session) = 0;

 protected:
  friend class FrameTimings;

  // Called when the frame drawn by RenderFrame() has been
  // presented to the display.
  virtual void OnFramePresented(const FrameTimings& timings) = 0;

  // Called when the frame drawn by RenderFrame() has finished rendering.
  virtual void OnFrameRendered(const FrameTimings& timings) = 0;
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_ENGINE_FRAME_SCHEDULER_H_
