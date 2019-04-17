// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/engine/default_frame_scheduler.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/async/time.h>
#include <trace/event.h>
#include <zircon/syscalls.h>

#include "garnet/lib/ui/gfx/displays/display.h"
#include "garnet/lib/ui/gfx/engine/frame_timings.h"
#include "src/lib/fxl/logging.h"

namespace scenic_impl {
namespace gfx {

DefaultFrameScheduler::DefaultFrameScheduler(const Display* display,
                                             inspect::Object inspect_object)
    : dispatcher_(async_get_default_dispatcher()),
      display_(display),
      inspect_object_(std::move(inspect_object)),
      weak_factory_(this) {
  outstanding_frames_.reserve(kMaxOutstandingFrames);

  inspect_frame_number_ =
      inspect_object_.CreateUIntMetric("most_recent_frame_number", frame_number_);
  inspect_last_successful_update_start_time_ = inspect_object_.CreateUIntMetric(
        "inspect_last_successful_update_start_time_", 0);
  inspect_last_successful_render_start_time_ = inspect_object_.CreateUIntMetric(
        "inspect_last_successful_render_start_time_", 0);
}

DefaultFrameScheduler::~DefaultFrameScheduler() {}

void DefaultFrameScheduler::OnFrameRendered(const FrameTimings& timings) {
  TRACE_INSTANT("gfx", "DefaultFrameScheduler::OnFrameRendered",
                TRACE_SCOPE_PROCESS, "Timestamp",
                timings.rendering_finished_time(), "frame_number",
                timings.frame_number());
}

void DefaultFrameScheduler::SetRenderContinuously(bool render_continuously) {
  render_continuously_ = render_continuously;
  if (render_continuously_) {
    RequestFrame();
  }
}

zx_time_t DefaultFrameScheduler::PredictRequiredFrameRenderTime() const {
  // TODO(MZ-400): more sophisticated prediction.  This might require more info,
  // e.g. about how many compositors will be rendering scenes, at what
  // resolutions, etc.
  constexpr zx_time_t kHardcodedPrediction = 8'000'000;  // 8ms
  return kHardcodedPrediction;
}

std::pair<zx_time_t, zx_time_t>
DefaultFrameScheduler::ComputePresentationAndWakeupTimesForTargetTime(
    const zx_time_t requested_presentation_time) const {
  const zx_time_t last_vsync_time = display_->GetLastVsyncTime();
  const zx_duration_t vsync_interval = display_->GetVsyncInterval();
  const zx_time_t now = async_now(dispatcher_);
  const zx_duration_t required_render_time = PredictRequiredFrameRenderTime();

  // Compute the number of full vsync intervals between the last vsync and the
  // requested presentation time.  Notes:
  //   - The requested time might be earlier than the last vsync time,
  //     for example when client content is a bit late.
  //   - We subtract a nanosecond before computing the number of intervals, to
  //     avoid an off-by-one error in the common case where a client computes a
  //     a desired presentation time based on a previously-received actual
  //     presentation time.
  uint64_t num_intervals =
      1 + (requested_presentation_time <= last_vsync_time
               ? 0
               : (requested_presentation_time - last_vsync_time - 1) /
                     vsync_interval);

  // Compute the target vsync/presentation time, and the time we would need to
  // start rendering to meet the target.
  zx_time_t target_presentation_time =
      last_vsync_time + (num_intervals * vsync_interval);
  zx_time_t wakeup_time = target_presentation_time - required_render_time;
  // Handle startup-time corner case: since monotonic clock starts at 0, there
  // will be underflow when required_render_time > target_presentation_time,
  // resulting in a *very* late wakeup time.
  while (required_render_time > target_presentation_time) {
    target_presentation_time += vsync_interval;
    wakeup_time = target_presentation_time - required_render_time;
  }

  // If it's too late to start rendering, delay a frame until there is enough
  // time.
  while (wakeup_time <= now) {
    target_presentation_time += vsync_interval;
    wakeup_time += vsync_interval;
  }

#if SCENIC_IGNORE_VSYNC
  return std::make_pair(now, now);
#else
  return std::make_pair(target_presentation_time, wakeup_time);
#endif
}

void DefaultFrameScheduler::RequestFrame() {
  FXL_DCHECK(!updatable_sessions_.empty() || render_continuously_ ||
             render_pending_);

  // Logging the first few frames to find common startup bugs.
  if (frame_number_ < 5) {
    FXL_LOG(INFO) << "DefaultFrameScheduler::RequestFrame";
  }

  auto requested_presentation_time =
      render_continuously_ || render_pending_
          ? 0
          : updatable_sessions_.top().requested_presentation_time;

  auto next_times = ComputePresentationAndWakeupTimesForTargetTime(
      requested_presentation_time);
  auto new_presentation_time = next_times.first;
  auto new_wakeup_time = next_times.second;

  // If there is no render waiting we should schedule a frame.
  // Likewise, if newly predicted wake up time is earlier than the current one
  // then we need to reschedule the next wake up.
  if (!frame_render_task_.is_pending() || new_wakeup_time < wakeup_time_) {
    frame_render_task_.Cancel();

    wakeup_time_ = new_wakeup_time;
    next_presentation_time_ = new_presentation_time;
    frame_render_task_.PostForTime(dispatcher_, zx::time(wakeup_time_));
  }
}

void DefaultFrameScheduler::MaybeRenderFrame(async_dispatcher_t*,
                                             async::TaskBase*, zx_status_t) {
  auto presentation_time = next_presentation_time_;
  TRACE_DURATION("gfx", "FrameScheduler::MaybeRenderFrame", "presentation_time",
                 presentation_time);

  // Logging the first few frames to find common startup bugs.
  if (frame_number_ < 5) {
    FXL_LOG(INFO)
        << "DefaultFrameScheduler::MaybeRenderFrame presentation_time="
        << presentation_time << " wakeup_time=" << wakeup_time_
        << " frame_number=" << frame_number_;
  }

  FXL_DCHECK(delegate_.frame_renderer);
  FXL_DCHECK(delegate_.session_updater);

  // Apply all updates
  const zx_time_t update_start_time = async_now(dispatcher_);
  bool any_updates_were_applied =
      ApplyScheduledSessionUpdates(presentation_time);

  if (any_updates_were_applied) {
    inspect_last_successful_update_start_time_.Set(update_start_time);
  }

  if (!any_updates_were_applied && !render_pending_ && !render_continuously_) {
    // If necessary, schedule another frame.
    if (!updatable_sessions_.empty()) {
      RequestFrame();
    }
    return;
  }

  // Some updates were applied; we interpret this to mean that the scene may
  // have changed, and therefore needs to be rendered.
  // TODO(SCN-1091): this is a very conservative approach that may result in
  // excessive rendering.

  if (currently_rendering_) {
    render_pending_ = true;
    return;
  }

  FXL_DCHECK(outstanding_frames_.size() < kMaxOutstandingFrames);

  // Logging the first few frames to find common startup bugs.
  if (frame_number_ < 5) {
    FXL_LOG(INFO)
        << "DefaultFrameScheduler: calling RenderFrame presentation_time="
        << presentation_time << " frame_number=" << frame_number_;
  }

  TRACE_INSTANT("gfx", "Render start", TRACE_SCOPE_PROCESS,
                "Expected presentation time", presentation_time, "frame_number",
                frame_number_);

  delegate_.session_updater->NewFrame();

  auto frame_timings =
      fxl::MakeRefCounted<FrameTimings>(this, frame_number_, presentation_time);
  inspect_frame_number_.Set(frame_number_);

  // Render the frame.
  currently_rendering_ =
      delegate_.frame_renderer->RenderFrame(frame_timings, presentation_time);
  if (currently_rendering_) {
    outstanding_frames_.push_back(frame_timings);
    render_pending_ = false;

    inspect_last_successful_render_start_time_.Set(presentation_time);
  } else {
    // TODO(SCN-1344): Handle failed rendering somehow.
    FXL_LOG(WARNING)
        << "RenderFrame failed. "
        << "There may not be any calls to OnFrameRendered or OnFramePresented, "
        << "and no callbacks may be invoked.";
  }

  ++frame_number_;

  // If necessary, schedule another frame.
  if (!updatable_sessions_.empty()) {
    RequestFrame();
  }
}

void DefaultFrameScheduler::ScheduleUpdateForSession(
    zx_time_t presentation_time, scenic_impl::SessionId session_id) {
  updatable_sessions_.push({.session_id = session_id,
                            .requested_presentation_time = presentation_time});

  // Logging the first few frames to find common startup bugs.
  if (frame_number_ < 5) {
    FXL_LOG(INFO)
        << "DefaultFrameScheduler::ScheduleUpdateForSession session_id: "
        << session_id << " presentation_time: " << presentation_time;
  }

  RequestFrame();
}

bool DefaultFrameScheduler::ApplyScheduledSessionUpdates(
    zx_time_t presentation_time) {
  FXL_DCHECK(delegate_.session_updater);

  // Logging the first few frames to find common startup bugs.
  if (frame_number_ < 5) {
    FXL_LOG(INFO) << "DefaultFrameScheduler::ApplyScheduledSessionUpdates "
                     "presentation_time="
                  << presentation_time << " frame_number=" << frame_number_;
  }
  TRACE_DURATION("gfx", "ApplyScheduledSessionUpdates", "time",
                 presentation_time);

  std::unordered_set<SessionId> sessions_to_update;
  while (!updatable_sessions_.empty() &&
         updatable_sessions_.top().requested_presentation_time <
             presentation_time) {
    sessions_to_update.insert(updatable_sessions_.top().session_id);
    updatable_sessions_.pop();
  }

  auto update_results = delegate_.session_updater->UpdateSessions(
      std::move(sessions_to_update), presentation_time);

  // Push updates that didn't have their fences ready back onto the queue to be
  // retried next frame.
  for (auto session_id : update_results.sessions_to_reschedule) {
    updatable_sessions_.push(
        {.session_id = session_id,
         .requested_presentation_time =
             presentation_time + display_->GetVsyncInterval()});
  }

  return update_results.needs_render;
}

void DefaultFrameScheduler::OnFramePresented(const FrameTimings& timings) {
  if (frame_number_ < 5) {
    FXL_LOG(INFO) << "DefaultFrameScheduler::OnFramePresented"
                  << " frame_number=" << timings.frame_number();
  }

  FXL_DCHECK(!outstanding_frames_.empty());
  // TODO(MZ-400): how should we handle this case?  It is theoretically
  // possible, but if if it happens then it means that the EventTimestamper is
  // receiving signals out-of-order and is therefore generating bogus data.
  FXL_DCHECK(outstanding_frames_[0].get() == &timings) << "out-of-order.";

  if (timings.frame_was_dropped()) {
    TRACE_INSTANT("gfx", "FrameDropped", TRACE_SCOPE_PROCESS, "frame_number",
                  timings.frame_number());
  } else {
    if (TRACE_CATEGORY_ENABLED("gfx")) {
      // Log trace data.
      // TODO(MZ-400): just pass the whole Frame to a listener.
      zx_duration_t target_vs_actual =
          timings.actual_presentation_time() - timings.target_presentation_time();

      zx_time_t now = async_now(dispatcher_);
      FXL_DCHECK(now >= timings.actual_presentation_time());
      zx_duration_t elapsed_since_presentation =
          now - timings.actual_presentation_time();

      TRACE_INSTANT("gfx", "FramePresented", TRACE_SCOPE_PROCESS, "frame_number",
                    timings.frame_number(), "presentation time",
                    timings.actual_presentation_time(), "target time missed by",
                    target_vs_actual, "elapsed time since presentation",
                    elapsed_since_presentation);
    }

    FXL_DCHECK(delegate_.session_updater);
    auto presentation_info = fuchsia::images::PresentationInfo();
    presentation_info.presentation_time = timings.actual_presentation_time();
    presentation_info.presentation_interval = display_->GetVsyncInterval();
    delegate_.session_updater->SignalSuccessfulPresentCallbacks(
        std::move(presentation_info));
  }

  // Pop the front Frame off the queue.
  for (size_t i = 1; i < outstanding_frames_.size(); ++i) {
    outstanding_frames_[i - 1] = std::move(outstanding_frames_[i]);
  }
  outstanding_frames_.resize(outstanding_frames_.size() - 1);

  currently_rendering_ = false;
  if (render_continuously_ || render_pending_) {
    RequestFrame();
  }
}

}  // namespace gfx
}  // namespace scenic_impl
