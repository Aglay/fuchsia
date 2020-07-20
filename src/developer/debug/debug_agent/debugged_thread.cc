// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/debugged_thread.h"

#include <inttypes.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>
#include <zircon/status.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/exception.h>

#include <memory>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/debug_agent.h"
#include "src/developer/debug/debug_agent/debugged_process.h"
#include "src/developer/debug/debug_agent/hardware_breakpoint.h"
#include "src/developer/debug/debug_agent/process_breakpoint.h"
#include "src/developer/debug/debug_agent/software_breakpoint.h"
#include "src/developer/debug/debug_agent/unwind.h"
#include "src/developer/debug/debug_agent/watchpoint.h"
#include "src/developer/debug/ipc/agent_protocol.h"
#include "src/developer/debug/ipc/message_reader.h"
#include "src/developer/debug/ipc/message_writer.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/platform_message_loop.h"
#include "src/developer/debug/shared/stream_buffer.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_agent {

namespace {

// Used to have better context upon reading the debug logs.
std::string ThreadPreamble(const DebuggedThread* thread) {
  return fxl::StringPrintf("[Pr: %lu (%s), T: %lu] ", thread->process()->koid(),
                           thread->process()->process_handle().GetName().c_str(), thread->koid());
}

// TODO(donosoc): Move this to a more generic place (probably shared) where it
//                can be used by other code.
const char* ExceptionTypeToString(uint32_t type) {
  switch (type) {
    case ZX_EXCP_GENERAL:
      return "ZX_EXCP_GENERAL";
    case ZX_EXCP_FATAL_PAGE_FAULT:
      return "ZX_EXCP_FATAL_PAGE_FAULT";
    case ZX_EXCP_UNDEFINED_INSTRUCTION:
      return "ZX_EXCP_UNDEFINED_INSTRUCTION";
    case ZX_EXCP_SW_BREAKPOINT:
      return "ZX_EXCP_SW_BREAKPOINT";
    case ZX_EXCP_HW_BREAKPOINT:
      return "ZX_EXCP_HW_BREAKPOINT";
    case ZX_EXCP_UNALIGNED_ACCESS:
      return "ZX_EXCP_UNALIGNED_ACCESS";
    default:
      break;
  }

  return "<unknown>";
}

void LogHitBreakpoint(debug_ipc::FileLineFunction location, const DebuggedThread* thread,
                      ProcessBreakpoint* process_breakpoint, uint64_t address) {
  if (!debug_ipc::IsDebugModeActive())
    return;

  std::stringstream ss;
  ss << ThreadPreamble(thread) << "Hit SW breakpoint on 0x" << std::hex << address << " for: ";
  for (Breakpoint* breakpoint : process_breakpoint->breakpoints()) {
    ss << breakpoint->settings().name << ", ";
  }

  DEBUG_LOG_WITH_LOCATION(Thread, location) << ss.str();
}

void LogExceptionNotification(debug_ipc::FileLineFunction location, const DebuggedThread* thread,
                              const debug_ipc::NotifyException& exception) {
  if (!debug_ipc::IsDebugModeActive())
    return;

  std::stringstream ss;
  ss << ThreadPreamble(thread) << "Notifying exception "
     << debug_ipc::ExceptionTypeToString(exception.type) << ". ";
  ss << "Breakpoints hit: ";
  int count = 0;
  for (auto& bp : exception.hit_breakpoints) {
    if (count > 0)
      ss << ", ";

    ss << bp.id;
    if (bp.should_delete)
      ss << " (delete)";
  }

  DEBUG_LOG_WITH_LOCATION(Thread, location) << ss.str();
}

}  // namespace

// DebuggedThread::SuspendToken --------------------------------------------------------------------

DebuggedThread::SuspendToken::SuspendToken(DebuggedThread* thread) : thread_(thread->GetWeakPtr()) {
  thread->IncreaseSuspend();
}

DebuggedThread::SuspendToken::~SuspendToken() {
  if (!thread_)
    return;
  thread_->DecreaseSuspend();
}

// DebuggedThread ----------------------------------------------------------------------------------

DebuggedThread::DebuggedThread(DebugAgent* debug_agent, DebuggedProcess* process,
                               std::unique_ptr<ThreadHandle> handle,
                               ThreadCreationOption creation_option,
                               std::unique_ptr<ExceptionHandle> exception)
    : thread_handle_(std::move(handle)),
      debug_agent_(debug_agent),
      process_(process),
      exception_handle_(std::move(exception)),
      weak_factory_(this) {
  switch (creation_option) {
    case ThreadCreationOption::kRunningKeepRunning:
      // do nothing
      break;
    case ThreadCreationOption::kSuspendedKeepSuspended:
      break;
    case ThreadCreationOption::kSuspendedShouldRun:
      ResumeException();
      break;
  }
}

DebuggedThread::~DebuggedThread() = default;

fxl::WeakPtr<DebuggedThread> DebuggedThread::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

void DebuggedThread::OnException(std::unique_ptr<ExceptionHandle> exception_handle) {
  exception_handle_ = std::move(exception_handle);

  debug_ipc::NotifyException exception;
  exception.type = exception_handle_->GetType(*thread_handle_);
  exception.exception = thread_handle_->GetExceptionRecord();

  auto strategy = exception_handle_->GetStrategy();
  if (strategy.is_error()) {
    FX_LOGS(WARNING) << "Could not determine exception strategy: "
                     << zx_status_get_string(strategy.error_value());
    return;
  }
  exception.exception.second_chance = (strategy.value() == ZX_EXCEPTION_STRATEGY_SECOND_CHANCE);

  std::optional<GeneralRegisters> regs = thread_handle_->GetGeneralRegisters();
  if (!regs) {
    // This can happen, for example, if the thread was killed during the time the exception message
    // was waiting to be delivered to us.
    FX_LOGS(WARNING) << "Could not read registers from thread.";
    return;
  }

  DEBUG_LOG(Thread) << ThreadPreamble(this) << "Exception @ 0x" << std::hex << regs->ip()
                    << std::dec << ": " << debug_ipc::ExceptionTypeToString(exception.type);

  switch (exception.type) {
    case debug_ipc::ExceptionType::kSingleStep:
      return HandleSingleStep(&exception, *regs);
    case debug_ipc::ExceptionType::kSoftwareBreakpoint:
      return HandleSoftwareBreakpoint(&exception, *regs);
    case debug_ipc::ExceptionType::kHardwareBreakpoint:
      return HandleHardwareBreakpoint(&exception, *regs);
    case debug_ipc::ExceptionType::kWatchpoint:
      return HandleWatchpoint(&exception, *regs);
    case debug_ipc::ExceptionType::kNone:
    case debug_ipc::ExceptionType::kLast:
      break;
    // TODO(donosoc): Should synthetic be general or invalid?
    case debug_ipc::ExceptionType::kSynthetic:
    default:
      return HandleGeneralException(&exception, *regs);
  }

  FX_NOTREACHED() << "Invalid exception notification type: "
                  << debug_ipc::ExceptionTypeToString(exception.type);

  // The exception was unhandled, so we close it so that the system can run its
  // course. The destructor would've done it anyway, but being explicit helps
  // readability.
  exception_handle_ = nullptr;
}

void DebuggedThread::HandleSingleStep(debug_ipc::NotifyException* exception,
                                      const GeneralRegisters& regs) {
  if (current_breakpoint_) {
    DEBUG_LOG(Thread) << ThreadPreamble(this) << "Ending single stepped over 0x" << std::hex
                      << current_breakpoint_->address();
    // Getting here means that the thread is done stepping over a breakpoint.
    // Depending on whether others threads are stepping over the breakpoints, this thread might be
    // suspended (waiting for other threads to step over).
    // This means that we cannot resume from suspension here, as the breakpoint is owning the
    // thread "run-lifetime".
    //
    // We can, though, resume from the exception, as effectively we already handled the single-step
    // exception, so there is no more need to keep the thread in an excepted state. The suspend
    // handle will take care of keeping the thread stopped.
    //
    // NOTE: It's important to resume the exception *after* telling the breakpoint we are done going
    //       over it. This is because in the case that there are no other threads queued (the normal
    //       case), it produces a window between resuming the exception and suspending the thread
    //       to reinstall the breakpointer, which could make the thread miss the exception. By
    //       keeping the exception until *after* the breakpoint has been told to step over, we
    //       ensure that any installs have already occured and thus the thread won't miss any
    //       breakpoints.
    thread_handle_->SetSingleStep(debug_ipc::ResumeRequest::MakesStep(run_mode_));
    current_breakpoint_->EndStepOver(this);
    current_breakpoint_ = nullptr;
    ResumeException();
    return;
  }

  if (!debug_ipc::ResumeRequest::MakesStep(run_mode_)) {
    // This could be due to a race where the user was previously single
    // stepping and then requested a continue or forward before the single
    // stepping completed. It could also be a breakpoint that was deleted while
    // in the process of single-stepping over it. In both cases, the
    // least confusing thing is to resume automatically (since forwarding the
    // single step exception to the debugged program makes no sense).
    DEBUG_LOG(Thread) << ThreadPreamble(this) << "Single step without breakpoint. Continuing.";
    ResumeForRunMode();
    return;
  }

  // When stepping in a range, automatically continue as long as we're
  // still in range.
  if (run_mode_ == debug_ipc::ResumeRequest::How::kStepInRange &&
      regs.ip() >= step_in_range_begin_ && regs.ip() < step_in_range_end_) {
    DEBUG_LOG(Thread) << ThreadPreamble(this) << "Stepping in range. Continuing.";
    ResumeForRunMode();
    return;
  }

  DEBUG_LOG(Thread) << ThreadPreamble(this) << "Expected single step. Notifying.";
  SendExceptionNotification(exception, regs);
}

void DebuggedThread::HandleGeneralException(debug_ipc::NotifyException* exception,
                                            const GeneralRegisters& regs) {
  SendExceptionNotification(exception, regs);
}

void DebuggedThread::HandleSoftwareBreakpoint(debug_ipc::NotifyException* exception,
                                              GeneralRegisters& regs) {
  auto on_stop = UpdateForSoftwareBreakpoint(regs, exception->hit_breakpoints);
  switch (on_stop) {
    case OnStop::kIgnore:
      return;
    case OnStop::kNotify:
      SendExceptionNotification(exception, regs);
      return;
    case OnStop::kResume: {
      // We mark the thread as within an exception
      ResumeForRunMode();
      return;
    }
  }

  FX_NOTREACHED() << "Invalid OnStop.";
}

void DebuggedThread::HandleHardwareBreakpoint(debug_ipc::NotifyException* exception,
                                              GeneralRegisters& regs) {
  uint64_t breakpoint_address = arch::BreakpointInstructionForHardwareExceptionAddress(regs.ip());
  HardwareBreakpoint* found_bp = process_->FindHardwareBreakpoint(breakpoint_address);
  if (found_bp) {
    UpdateForHitProcessBreakpoint(debug_ipc::BreakpointType::kHardware, found_bp,
                                  exception->hit_breakpoints);
  } else {
    // Hit a hw debug exception that doesn't belong to any ProcessBreakpoint. This is probably a
    // race between the removal and the exception handler.
    regs.set_ip(breakpoint_address);
  }

  // The ProcessBreakpoint could've been deleted if it was a one-shot, so must not be derefereced
  // below this.
  found_bp = nullptr;
  SendExceptionNotification(exception, regs);
}

void DebuggedThread::HandleWatchpoint(debug_ipc::NotifyException* exception,
                                      const GeneralRegisters& regs) {
  std::optional<DebugRegisters> debug_regs = thread_handle_->GetDebugRegisters();
  if (!debug_regs) {
    DEBUG_LOG(Thread) << "Could not load debug registers to handle watchpoint.";
    return;
  }

  std::optional<WatchpointInfo> hit = debug_regs->DecodeHitWatchpoint();
  if (!hit) {
    // When no watchpoint matches this watchpoint, send the exception notification and let the
    // debugger frontend handle the exception.
    DEBUG_LOG(Thread) << "Could not find watchpoint.";
    SendExceptionNotification(exception, regs);
    return;
  }

  DEBUG_LOG(Thread) << "Found watchpoint hit at 0x" << std::hex << hit->range.ToString()
                    << " on slot " << std::dec << hit->slot;

  // Comparison is by the base of the address range.
  Watchpoint* watchpoint = process_->FindWatchpoint(hit->range);
  if (!watchpoint) {
    DEBUG_LOG(Thread) << "Could not find watchpoint for range " << hit->range.ToString();
    SendExceptionNotification(exception, regs);
    return;
  }

  // TODO(donosoc): Plumb in R/RW types.
  UpdateForHitProcessBreakpoint(watchpoint->Type(), watchpoint, exception->hit_breakpoints);
  // The ProcessBreakpoint could'be been deleted, so we cannot use it anymore.
  watchpoint = nullptr;
  SendExceptionNotification(exception, regs);
}

void DebuggedThread::SendExceptionNotification(debug_ipc::NotifyException* exception,
                                               const GeneralRegisters& regs) {
  exception->thread = GetThreadRecord(debug_ipc::ThreadRecord::StackAmount::kMinimal, regs);

  // Keep the thread suspended for the client.

  // TODO(brettw) suspend other threads in the process and other debugged
  // processes as desired.

  LogExceptionNotification(FROM_HERE, this, *exception);

  // Send notification.
  debug_ipc::MessageWriter writer;
  debug_ipc::WriteNotifyException(*exception, &writer);
  debug_agent_->stream()->Write(writer.MessageComplete());
}

void DebuggedThread::Resume(const debug_ipc::ResumeRequest& request) {
  DEBUG_LOG(Thread) << ThreadPreamble(this)
                    << "Resuming. Run mode: " << debug_ipc::ResumeRequest::HowToString(request.how)
                    << ", Range: [" << request.range_begin << ", " << request.range_end << ").";

  run_mode_ = request.how;
  step_in_range_begin_ = request.range_begin;
  step_in_range_end_ = request.range_end;

  ResumeForRunMode();
}

void DebuggedThread::ResumeException() {
  if (!IsInException()) {
    return;
  }

  if (run_mode_ == debug_ipc::ResumeRequest::How::kForwardAndContinue) {
    zx_status_t status = exception_handle_->SetStrategy(ZX_EXCEPTION_STRATEGY_SECOND_CHANCE);
    if (status != ZX_OK) {
      DEBUG_LOG(Thread) << ThreadPreamble(this) << "Failed to set exception as second-chance: "
                        << zx_status_get_string(status);
    }
  } else {
    zx_status_t status = exception_handle_->SetState(ZX_EXCEPTION_STATE_HANDLED);
    if (status != ZX_OK) {
      DEBUG_LOG(Thread) << ThreadPreamble(this)
                        << "Failed to set exception as handled: " << zx_status_get_string(status);
    }
  }
  exception_handle_ = nullptr;
}

void DebuggedThread::ResumeSuspension() {
  if (local_suspend_token_) {
    DEBUG_LOG(Thread) << ThreadPreamble(this) << "Resuming suspend token.";
  }
  local_suspend_token_.reset();
}

bool DebuggedThread::Suspend(bool synchronous) {
  if (local_suspend_token_) {
    // The thread could have an asynchronous suspend pending from before, but it might not
    // actually be suspended yet. If somebody requests a synchronous suspend, make sure we honor
    // that the thread is suspended before returning.
    if (synchronous)
      WaitForSuspension(DefaultSuspendDeadline());
    return false;
  }
  local_suspend_token_ = RefCountedSuspend(synchronous);

  // If there is only one count, we know that this was the token that did the suspension.
  return suspend_count_ == 1;
}

std::unique_ptr<DebuggedThread::SuspendToken> DebuggedThread::RefCountedSuspend(bool synchronous) {
  auto token = std::unique_ptr<SuspendToken>(new SuspendToken(this));

  if (synchronous)
    WaitForSuspension(DefaultSuspendDeadline());
  return token;
}

zx::time DebuggedThread::DefaultSuspendDeadline() {
  // Various events and environments can cause suspensions to take a long time, so this needs to
  // be a relatively long time. We don't generally expect error cases that take infinitely long so
  // there isn't much downside of a long timeout.
  return zx::deadline_after(zx::msec(100));
}

bool DebuggedThread::WaitForSuspension(zx::time deadline) {
  // The thread could already be suspended. This bypasses a wait cycle in that case.
  if (thread_handle_->GetState().state == debug_ipc::ThreadRecord::State::kSuspended)
    return true;  // Already suspended, success.

  // This function is complex because a thread in an exception state can't be suspended (ZX-3772).
  // Delivery of exceptions are queued on the exception port so our cached state may be stale, and
  // exceptions can also race with our suspend call.
  //
  // To manually stress-test this code, write a one-line infinite loop:
  //   volatile bool done = false;
  //   while (!done) {}
  // and step over it with "next". This will cause an infinite flood of single-step exceptions as
  // fast as the debugger can process them. Pausing after doing the "next" will trigger a
  // suspension and is more likely to race with an exception.

  // If an exception happens before the suspend does, we'll never get the suspend signal and will
  // end up waiting for the entire timeout just to be able to tell the difference between
  // suspended and exception. To avoid waiting for a long timeout to tell the difference, wait for
  // short timeouts multiple times.
  auto poll_time = zx::msec(10);
  zx_status_t status = ZX_OK;
  do {
    // Before waiting, check the thread state from the kernel because of queue described above.
    if (thread_handle_->GetState().is_blocked_on_exception())
      return true;

    zx_signals_t observed;
    status = thread_handle_->GetNativeHandle().wait_one(ZX_THREAD_SUSPENDED,
                                                        zx::deadline_after(poll_time), &observed);
    if (status == ZX_OK && (observed & ZX_THREAD_SUSPENDED))
      return true;

  } while (status == ZX_ERR_TIMED_OUT && zx::clock::get_monotonic() < deadline);
  return false;
}

// Note that everything in this function is racy because the thread state can change at any time,
// even while processing an exception (an external program can kill it out from under us).
debug_ipc::ThreadRecord DebuggedThread::GetThreadRecord(
    debug_ipc::ThreadRecord::StackAmount stack_amount, std::optional<GeneralRegisters> regs) const {
  debug_ipc::ThreadRecord record = thread_handle_->GetThreadRecord(process_->koid());

  // Unwind the stack if requested. This requires the registers which are available when suspended
  // or blocked in an exception.
  if ((record.state == debug_ipc::ThreadRecord::State::kSuspended ||
       (record.state == debug_ipc::ThreadRecord::State::kBlocked &&
        record.blocked_reason == debug_ipc::ThreadRecord::BlockedReason::kException)) &&
      stack_amount != debug_ipc::ThreadRecord::StackAmount::kNone) {
    // Only record this when we actually attempt to query the stack.
    record.stack_amount = stack_amount;

    // The registers are required, fetch them if the caller didn't provide.
    if (!regs)
      regs = thread_handle_->GetGeneralRegisters();  // Note this could still fail.

    if (regs) {
      // Minimal stacks are 2 (current frame and calling one). Full stacks max out at 256 to
      // prevent edge cases, especially around corrupted stacks.
      uint32_t max_stack_depth =
          stack_amount == debug_ipc::ThreadRecord::StackAmount::kMinimal ? 2 : 256;

      UnwindStack(process_->process_handle(), process_->dl_debug_addr(), thread_handle(), *regs,
                  max_stack_depth, &record.frames);
    }
  } else {
    // Didn't bother querying the stack.
    record.stack_amount = debug_ipc::ThreadRecord::StackAmount::kNone;
  }
  return record;
}

std::vector<debug_ipc::Register> DebuggedThread::ReadRegisters(
    const std::vector<debug_ipc::RegisterCategory>& cats_to_get) const {
  return thread_handle_->ReadRegisters(cats_to_get);
}

std::vector<debug_ipc::Register> DebuggedThread::WriteRegisters(
    const std::vector<debug_ipc::Register>& regs) {
  std::vector<debug_ipc::Register> written = thread_handle_->WriteRegisters(regs);

  // If we're updating the instruction pointer directly, current state is no longer valid.
  // Specifically, if we're currently on a breakpoint, we have to now know the fact that we're no
  // longer in a breakpoint.
  //
  // This is necessary to avoid the single-stepping logic that the thread does when resuming from
  // a breakpoint.
  bool rip_change = false;
  debug_ipc::RegisterID rip_id =
      GetSpecialRegisterID(arch::GetCurrentArch(), debug_ipc::SpecialRegisterType::kIP);
  for (const debug_ipc::Register& reg : regs) {
    if (reg.id == rip_id) {
      rip_change = true;
      break;
    }
  }
  if (rip_change)
    current_breakpoint_ = nullptr;

  return written;
}

void DebuggedThread::SendThreadNotification() const {
  DEBUG_LOG(Thread) << ThreadPreamble(this) << "Sending starting notification.";
  debug_ipc::NotifyThread notify;
  notify.record = GetThreadRecord(debug_ipc::ThreadRecord::StackAmount::kMinimal);

  debug_ipc::MessageWriter writer;
  debug_ipc::WriteNotifyThread(debug_ipc::MsgHeader::Type::kNotifyThreadStarting, notify, &writer);
  debug_agent_->stream()->Write(writer.MessageComplete());
}

void DebuggedThread::WillDeleteProcessBreakpoint(ProcessBreakpoint* bp) {
  if (current_breakpoint_ == bp)
    current_breakpoint_ = nullptr;
}

DebuggedThread::OnStop DebuggedThread::UpdateForSoftwareBreakpoint(
    GeneralRegisters& regs, std::vector<debug_ipc::BreakpointStats>& hit_breakpoints) {
  // Get the correct address where the CPU is after hitting a breakpoint
  // (this is architecture specific).
  uint64_t breakpoint_address = arch::BreakpointInstructionForSoftwareExceptionAddress(regs.ip());

  SoftwareBreakpoint* found_bp = process_->FindSoftwareBreakpoint(breakpoint_address);
  if (found_bp) {
    LogHitBreakpoint(FROM_HERE, this, found_bp, breakpoint_address);

    FixSoftwareBreakpointAddress(found_bp, regs);

    // When hitting a breakpoint, we need to check if indeed this exception
    // should apply to this thread or not.
    if (!found_bp->ShouldHitThread(koid())) {
      DEBUG_LOG(Thread) << ThreadPreamble(this) << "SW Breakpoint not for me. Ignoring.";
      // The way to go over is to step over the breakpoint as one would over a resume.
      current_breakpoint_ = found_bp;
      return OnStop::kResume;
    }

    UpdateForHitProcessBreakpoint(debug_ipc::BreakpointType::kSoftware, found_bp, hit_breakpoints);

    // The found_bp could have been deleted if it was a one-shot, so must
    // not be dereferenced below this.
    found_bp = nullptr;
  } else {
    // Hit a software breakpoint that doesn't correspond to any current breakpoint.
    if (IsBreakpointInstructionAtAddress(breakpoint_address)) {
      // The breakpoint is a hardcoded instruction in the program code. In this case we want to
      // continue from the following instruction since the breakpoint instruction will never go
      // away.
      regs.set_ip(arch::NextInstructionForSoftwareExceptionAddress(regs.ip()));
      thread_handle_->SetGeneralRegisters(regs);

      if (!process_->dl_debug_addr() && process_->RegisterDebugState()) {
        DEBUG_LOG(Thread) << ThreadPreamble(this) << "Found ld.so breakpoint. Sending modules.";
        // This breakpoint was the explicit breakpoint ld.so executes to notify us that the loader
        // is ready (see DebuggerProcess::RegisterDebugState).
        //
        // Send the current module list and silently keep this thread stopped. The client will
        // explicitly resume this thread when it's ready to continue (it will need to load symbols
        // for the modules and may need to set breakpoints based on them).
        std::vector<uint64_t> paused_threads;
        paused_threads.push_back(koid());
        process_->SendModuleNotification(std::move(paused_threads));
        return OnStop::kIgnore;
      }
    } else {
      DEBUG_LOG(Thread) << ThreadPreamble(this) << "Hit non debugger SW breakpoint on 0x"
                        << std::hex << breakpoint_address;
      // Not a breakpoint instruction. Probably the breakpoint instruction used to be ours but its
      // removal raced with the exception handler. Resume from the instruction that used to be the
      // breakpoint.
      regs.set_ip(breakpoint_address);

      // Don't automatically continue execution here. A race for this should be unusual and maybe
      // something weird happened that caused an exception we're not set up to handle. Err on the
      // side of telling the user about the exception.
    }
  }
  return OnStop::kNotify;
}

void DebuggedThread::FixSoftwareBreakpointAddress(ProcessBreakpoint* process_breakpoint,
                                                  GeneralRegisters& regs) {
  // When the program hits one of our breakpoints, set the IP back to the exact address that
  // triggered the breakpoint. When the thread resumes, this is the address that it will resume
  // from (after putting back the original instruction), and will be what the client wants to
  // display to the user.
  regs.set_ip(process_breakpoint->address());
  thread_handle_->SetGeneralRegisters(regs);
}

void DebuggedThread::UpdateForHitProcessBreakpoint(
    debug_ipc::BreakpointType exception_type, ProcessBreakpoint* process_breakpoint,
    std::vector<debug_ipc::BreakpointStats>& hit_breakpoints) {
  current_breakpoint_ = process_breakpoint;

  process_breakpoint->OnHit(exception_type, &hit_breakpoints);

  // Delete any one-shot breakpoints. Since there can be multiple Breakpoints
  // (some one-shot, some not) referring to the current ProcessBreakpoint,
  // this operation could delete the ProcessBreakpoint or it could not. If it
  // does, our observer will be told and current_breakpoint_ will be cleared.
  for (const auto& stats : hit_breakpoints) {
    if (stats.should_delete)
      process_->debug_agent()->RemoveBreakpoint(stats.id);
  }
}

bool DebuggedThread::IsBreakpointInstructionAtAddress(uint64_t address) const {
  arch::BreakInstructionType instruction = 0;
  size_t bytes_read = 0;
  if (process_->process_handle().ReadMemory(address, &instruction, sizeof(instruction),
                                            &bytes_read) != ZX_OK ||
      bytes_read != sizeof(instruction))
    return false;
  return arch::IsBreakpointInstruction(instruction);
}

void DebuggedThread::ResumeForRunMode() {
  // We check if we're set to currently step over a breakpoint. If so we need to do some special
  // handling, as going over a breakpoint is always a single-step operation.
  // After that we can continue according to the set run-mode.
  if (IsInException() && current_breakpoint_) {
    DEBUG_LOG(Thread) << ThreadPreamble(this) << "Stepping over breakpoint: 0x" << std::hex
                      << current_breakpoint_->address();
    thread_handle_->SetSingleStep(true);
    current_breakpoint_->BeginStepOver(this);

    // In this case, the breakpoint takes control of the thread lifetime and has already set the
    // thread to resume.
    return;
  }

  // We're not handling the special "step over a breakpoint case". This is the normal resume case.
  // This could've been triggered by an internal resume (eg. triggered by a breakpoint), so we
  // need to check if the client actually wants this thread to resume.
  if (client_state_ == ClientState::kPaused)
    return;

  thread_handle_->SetSingleStep(debug_ipc::ResumeRequest::MakesStep(run_mode_));
  ResumeException();
  ResumeSuspension();
}

const char* DebuggedThread::ClientStateToString(ClientState client_state) {
  switch (client_state) {
    case ClientState::kRunning:
      return "Running";
    case ClientState::kPaused:
      return "Paused";
  }

  FX_NOTREACHED();
  return "<unknown>";
}

void DebuggedThread::IncreaseSuspend() {
  suspend_count_++;

  // We only need to keep one suspend token around.
  if (ref_counted_suspend_token_.is_valid())
    return;

  zx_status_t status = thread_handle_->GetNativeHandle().suspend(&ref_counted_suspend_token_);
  if (status != ZX_OK) {
    DEBUG_LOG(Thread) << ThreadPreamble(this)
                      << "Could not suspend: " << zx_status_get_string(status);
  }
}

void DebuggedThread::DecreaseSuspend() {
  suspend_count_--;
  FX_DCHECK(suspend_count_ >= 0);
  if (suspend_count_ > 0)
    return;
  ref_counted_suspend_token_.reset();
}

}  // namespace debug_agent
