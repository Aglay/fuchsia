// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/step_over_thread_controller.h"

#include "garnet/bin/zxdb/client/finish_thread_controller.h"
#include "garnet/bin/zxdb/client/frame.h"
#include "garnet/bin/zxdb/client/process.h"
#include "garnet/bin/zxdb/client/step_thread_controller.h"
#include "garnet/bin/zxdb/client/thread.h"
#include "garnet/bin/zxdb/common/address_ranges.h"
#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/symbols/line_details.h"
#include "garnet/bin/zxdb/symbols/process_symbols.h"
#include "lib/fxl/logging.h"

namespace zxdb {

StepOverThreadController::StepOverThreadController(StepMode mode)
    : step_mode_(mode),
      step_into_(std::make_unique<StepThreadController>(mode)) {
  FXL_DCHECK(mode != StepMode::kAddressRange);
}

StepOverThreadController::StepOverThreadController(AddressRanges range)
    : step_mode_(StepMode::kAddressRange),
      step_into_(std::make_unique<StepThreadController>(std::move(range))) {}

StepOverThreadController::~StepOverThreadController() = default;

void StepOverThreadController::InitWithThread(
    Thread* thread, std::function<void(const Err&)> cb) {
  set_thread(thread);

  if (thread->GetStack().empty()) {
    cb(Err("Can't step, no frames."));
    return;
  }

  // Save the info for the frame we're stepping inside of for future possible
  // stepping out.
  Stack& stack = thread->GetStack();
  frame_fingerprint_ = *stack.GetFrameFingerprint(0);
  if (step_mode_ == StepMode::kSourceLine) {
    // Always take the file/line from the frame rather than from LineDetails.
    // In the case of ambiguous inline locations, the LineDetails will contain
    // only the innermost inline frame's file/line, while the user could be
    // stepping at a higher level where the frame's file line was computed
    // synthetically from the inline call hierarchy.
    file_line_ = stack[0]->GetLocation().file_line();
    Log("Stepping over %s:%d", file_line_.file().c_str(), file_line_.line());
  }

  // Stepping in the function itself is managed by the StepInto controller.
  step_into_->InitWithThread(thread, std::move(cb));
}

ThreadController::ContinueOp StepOverThreadController::GetContinueOp() {
  if (finish_)
    return finish_->GetContinueOp();
  return step_into_->GetContinueOp();
}

ThreadController::StopOp StepOverThreadController::OnThreadStop(
    debug_ipc::NotifyException::Type stop_type,
    const std::vector<fxl::WeakPtr<Breakpoint>>& hit_breakpoints) {
  if (finish_) {
    // Currently trying to step out of a sub-frame.
    if (finish_->OnThreadStop(stop_type, hit_breakpoints) == kContinue) {
      // Not done stepping out, keep working on it.
      Log("Still not done stepping out of sub-frame.");
      return kContinue;
    }

    // Done stepping out. The "finish" operation is complete, but we may need
    // to resume single-stepping in the outer frame.
    Log("Done stepping out of sub-frame.");
    finish_.reset();

    // Pass the "none" exception type here to bypass checking the exception
    // type.
    //
    // TODO(DX-1058): this is wrong. If the program crashes while stepping this
    // might try to continue it. What we really want is a flag from the finish
    // controller to differentiate "stop because crazy stuff is happening" and
    // "stop because I reached my destination." The former implies we should
    // also stop, the latter implies we should continue with this logic and can
    // ignore the exception type.
    //
    // TODO(brettw) this re-uses the step-into controller that already reported
    // stop (causing us to do the "finish" operation). Once a controller
    // reports "stop" we should no re-use it. A new controller should be
    // created. Possibly the code below that creates a new step_into_
    // controller might be sufficient in which case this call can be deleted.
    if (step_into_->OnThreadStop(debug_ipc::NotifyException::Type::kNone, {}) ==
        kContinue) {
      Log("Still in range after stepping out.");
      return kContinue;
    }
  } else {
    if (step_into_->OnThreadStop(stop_type, {}) == kContinue) {
      Log("Still in range after stepping out.");
      return kContinue;
    }
  }

  // If we just stepped into and out of a function, we could end up on the same
  // line as we started on and the user expects "step over" to keep going in
  // that case.
  Stack& stack = thread()->GetStack();
  FrameFingerprint current_fingerprint =
      *thread()->GetStack().GetFrameFingerprint(0);
  if (step_mode_ == StepMode::kSourceLine &&
      current_fingerprint == frame_fingerprint_ &&
      file_line_ == stack[0]->GetLocation().file_line()) {
    // Same stack frame and same line number, do "step into" again.
    Log("Same line, doing a new StepController to keep going.");
    step_into_ = std::make_unique<StepThreadController>(StepMode::kSourceLine);
    step_into_->InitWithThread(thread(), [](const Err&){});
    // Pass no exception type or breakpoints because we just want the step
    // controller to evaluate the current position regardless of how we got
    // here.
    if (step_into_->OnThreadStop(debug_ipc::NotifyException::Type::kNone, {}) ==
        kContinue)
      return kContinue;

    // The step controller may have tweaked the stack, recompute the current
    // fingerprint.
    current_fingerprint = *thread()->GetStack().GetFrameFingerprint(0);
  }

  // If we get here the thread is no longer in range but could be in a sub-
  // frame that we need to step out of.
  if (!FrameFingerprint::Newer(current_fingerprint, frame_fingerprint_)) {
    Log("Neither in range nor in a newer frame.");
    return kStop;
  }

  if (stack.size() < 2) {
    Log("In a newer frame but there are not enough frames to step out.");
    return kStop;
  }

  // Got into a sub-frame. The calling code may have added a filter to stop
  // at one of them.
  if (subframe_should_stop_callback_) {
    if (subframe_should_stop_callback_(stack[0])) {
      // Don't set the ambiguous inline frame in this case because we're in
      // a subframe of the one we were originally stepping in.
      Log("should_stop callback returned true, stopping.");
      return kStop;
    } else {
      Log("should_stop callback returned false, continuing.");
    }
  }

  // Begin stepping out of the sub-frame. The "finish" command initialization
  // is technically asynchronous since it's waiting for the breakpoint to be
  // successfully set. Since we're supplying an address to run to instead of a
  // symbol, there isn't much that can go wrong other than the process could
  // be terminated out from under us or the memory is unmapped.
  //
  // These cases are catastrophic anyway so don't worry about those errors.
  // Waiting for a full round-trip to the debugged system for every function
  // call in a "next" command would slow everything down and make things
  // more complex. It also means that the thread may be stopped if the user
  // asks for the state in the middle of a "next" command which would be
  // surprising.
  //
  // Since the IPC will serialize the command, we know that successful
  // breakpoint sets will arrive before telling the thread to continue.
  Log("In a new frame, passing through to 'finish'.");
  finish_ = std::make_unique<FinishThreadController>(stack, 0);
  finish_->InitWithThread(thread(), [](const Err&) {});

  // Pass the "none" exception type here to bypass checking the exception
  // type.
  //
  // TODO(brettw) DX-1058 this is wrong, see similar comment above.
  return finish_->OnThreadStop(debug_ipc::NotifyException::Type::kNone, {});
}

}  // namespace zxdb
