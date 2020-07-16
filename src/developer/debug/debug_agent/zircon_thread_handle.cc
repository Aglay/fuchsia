// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/zircon_thread_handle.h"

#include <map>

#include "src/developer/debug/debug_agent/zircon_utils.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/zx_status.h"

#if defined(__x86_64__)
#include "src/developer/debug/debug_agent/arch_x64_helpers.h"
#endif

namespace debug_agent {

namespace {

debug_ipc::ThreadRecord::BlockedReason ThreadStateBlockedReasonToEnum(uint32_t state) {
  FX_DCHECK(ZX_THREAD_STATE_BASIC(state) == ZX_THREAD_STATE_BLOCKED);

  switch (state) {
    case ZX_THREAD_STATE_BLOCKED_EXCEPTION:
      return debug_ipc::ThreadRecord::BlockedReason::kException;
    case ZX_THREAD_STATE_BLOCKED_SLEEPING:
      return debug_ipc::ThreadRecord::BlockedReason::kSleeping;
    case ZX_THREAD_STATE_BLOCKED_FUTEX:
      return debug_ipc::ThreadRecord::BlockedReason::kFutex;
    case ZX_THREAD_STATE_BLOCKED_PORT:
      return debug_ipc::ThreadRecord::BlockedReason::kPort;
    case ZX_THREAD_STATE_BLOCKED_CHANNEL:
      return debug_ipc::ThreadRecord::BlockedReason::kChannel;
    case ZX_THREAD_STATE_BLOCKED_WAIT_ONE:
      return debug_ipc::ThreadRecord::BlockedReason::kWaitOne;
    case ZX_THREAD_STATE_BLOCKED_WAIT_MANY:
      return debug_ipc::ThreadRecord::BlockedReason::kWaitMany;
    case ZX_THREAD_STATE_BLOCKED_INTERRUPT:
      return debug_ipc::ThreadRecord::BlockedReason::kInterrupt;
    case ZX_THREAD_STATE_BLOCKED_PAGER:
      return debug_ipc::ThreadRecord::BlockedReason::kPager;
    default:
      FX_NOTREACHED();
      return debug_ipc::ThreadRecord::BlockedReason::kNotBlocked;
  }
}

ThreadHandle::State ThreadStateToEnums(uint32_t input) {
  struct Mapping {
    uint32_t int_state;
    debug_ipc::ThreadRecord::State enum_state;
  };
  static const Mapping mappings[] = {
      {ZX_THREAD_STATE_NEW, debug_ipc::ThreadRecord::State::kNew},
      {ZX_THREAD_STATE_RUNNING, debug_ipc::ThreadRecord::State::kRunning},
      {ZX_THREAD_STATE_SUSPENDED, debug_ipc::ThreadRecord::State::kSuspended},
      {ZX_THREAD_STATE_BLOCKED, debug_ipc::ThreadRecord::State::kBlocked},
      {ZX_THREAD_STATE_DYING, debug_ipc::ThreadRecord::State::kDying},
      {ZX_THREAD_STATE_DEAD, debug_ipc::ThreadRecord::State::kDead}};

  const uint32_t basic_state = ZX_THREAD_STATE_BASIC(input);
  for (const Mapping& mapping : mappings) {
    if (mapping.int_state == basic_state) {
      if (mapping.enum_state == debug_ipc::ThreadRecord::State::kBlocked)
        return ThreadHandle::State(mapping.enum_state, ThreadStateBlockedReasonToEnum(input));
      return ThreadHandle::State(mapping.enum_state);
    }
  }
  return ThreadHandle::State(debug_ipc::ThreadRecord::State::kDead);
}

}  // namespace

ZirconThreadHandle::ZirconThreadHandle(zx::thread t)
    : thread_koid_(zircon::KoidForObject(t)), thread_(std::move(t)) {}

std::string ZirconThreadHandle::GetName() const { return zircon::NameForObject(thread_); }

ThreadHandle::State ZirconThreadHandle::GetState() const {
  zx_info_thread info;
  if (thread_.get_info(ZX_INFO_THREAD, &info, sizeof(info), nullptr, nullptr) == ZX_OK)
    return ThreadStateToEnums(info.state);
  return State(debug_ipc::ThreadRecord::State::kDead);  // Assume failures mean the thread is dead.
}

debug_ipc::ExceptionRecord ZirconThreadHandle::GetExceptionRecord() const {
  zx_exception_report_t report = {};
  if (thread_.get_info(ZX_INFO_THREAD_EXCEPTION_REPORT, &report, sizeof(report), nullptr,
                       nullptr) == ZX_OK)
    return arch::FillExceptionRecord(report);
  return debug_ipc::ExceptionRecord();
}

zx::suspend_token ZirconThreadHandle::Suspend() {
  zx::suspend_token result;
  thread_.suspend(&result);
  return result;
}

debug_ipc::ThreadRecord ZirconThreadHandle::GetThreadRecord(zx_koid_t process_koid) const {
  debug_ipc::ThreadRecord record;
  record.process_koid = process_koid;
  record.thread_koid = thread_koid_;

  // Name.
  char name[ZX_MAX_NAME_LEN];
  if (thread_.get_property(ZX_PROP_NAME, name, sizeof(name)) == ZX_OK)
    record.name = name;

  // State (running, blocked, etc.).
  auto state = GetState();
  record.state = state.state;
  record.blocked_reason = state.blocked_reason;

  return record;
}

std::optional<GeneralRegisters> ZirconThreadHandle::GetGeneralRegisters() const {
  zx_thread_state_general_regs r;
  if (thread_.read_state(ZX_THREAD_STATE_GENERAL_REGS, &r, sizeof(r)) == ZX_OK)
    return GeneralRegisters(r);
  return std::nullopt;
}

void ZirconThreadHandle::SetGeneralRegisters(const GeneralRegisters& regs) {
  thread_.write_state(ZX_THREAD_STATE_GENERAL_REGS, &regs.GetNativeRegisters(),
                      sizeof(zx_thread_state_general_regs));
}

std::optional<DebugRegisters> ZirconThreadHandle::GetDebugRegisters() const {
  zx_thread_state_debug_regs regs;
  if (thread_.read_state(ZX_THREAD_STATE_DEBUG_REGS, &regs, sizeof(regs)) == ZX_OK)
    return DebugRegisters(regs);
  return std::nullopt;
}

bool ZirconThreadHandle::SetDebugRegisters(const DebugRegisters& regs) {
  return thread_.write_state(ZX_THREAD_STATE_DEBUG_REGS, &regs.GetNativeRegisters(),
                             sizeof(zx_thread_state_debug_regs)) == ZX_OK;
}

void ZirconThreadHandle::SetSingleStep(bool single_step) {
  zx_thread_state_single_step_t value = single_step ? 1 : 0;
  // This could fail for legitimate reasons, like the process could have just closed the thread.
  thread_.write_state(ZX_THREAD_STATE_SINGLE_STEP, &value, sizeof(value));
}

std::vector<debug_ipc::Register> ZirconThreadHandle::ReadRegisters(
    const std::vector<debug_ipc::RegisterCategory>& cats_to_get) const {
  std::vector<debug_ipc::Register> regs;
  for (const auto& cat_type : cats_to_get)
    arch::ReadRegisters(thread_, cat_type, regs);
  return regs;
}

std::vector<debug_ipc::Register> ZirconThreadHandle::WriteRegisters(
    const std::vector<debug_ipc::Register>& regs) {
  std::vector<debug_ipc::Register> written;

  // Figure out which registers will change.
  std::map<debug_ipc::RegisterCategory, std::vector<debug_ipc::Register>> categories;
  for (const debug_ipc::Register& reg : regs) {
    auto cat_type = debug_ipc::RegisterIDToCategory(reg.id);
    if (cat_type == debug_ipc::RegisterCategory::kNone) {
      FX_LOGS(WARNING) << "Attempting to change register without category: "
                       << RegisterIDToString(reg.id);
      continue;
    }

    categories[cat_type].push_back(reg);
  }

  for (const auto& [cat_type, cat_regs] : categories) {
    FX_DCHECK(cat_type != debug_ipc::RegisterCategory::kNone);
    if (auto res = arch::WriteRegisters(thread_, cat_type, cat_regs); res != ZX_OK) {
      FX_LOGS(WARNING) << "Could not write category "
                       << debug_ipc::RegisterCategoryToString(cat_type) << ": "
                       << debug_ipc::ZxStatusToString(res);
    }

    if (auto res = arch::ReadRegisters(thread_, cat_type, written); res != ZX_OK) {
      FX_LOGS(WARNING) << "Could not read category "
                       << debug_ipc::RegisterCategoryToString(cat_type) << ": "
                       << debug_ipc::ZxStatusToString(res);
    }
  }

  return written;
}

bool ZirconThreadHandle::InstallHWBreakpoint(uint64_t address) {
  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return false;
  DEBUG_LOG(Thread) << "Before installing HW breakpoint:" << std::endl << regs->ToString();

  if (!regs->SetHWBreakpoint(address))
    return false;

  DEBUG_LOG(Thread) << "After installing HW breakpoint: " << std::endl << regs->ToString();
  return SetDebugRegisters(*regs);
}

bool ZirconThreadHandle::UninstallHWBreakpoint(uint64_t address) {
  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return false;
  DEBUG_LOG(Thread) << "Before uninstalling HW breakpoint:" << std::endl << regs->ToString();

  if (!regs->RemoveHWBreakpoint(address))
    return false;

  DEBUG_LOG(Thread) << "After uninstalling HW breakpoint: " << std::endl << regs->ToString();
  return SetDebugRegisters(*regs);
}

std::optional<WatchpointInfo> ZirconThreadHandle::InstallWatchpoint(
    debug_ipc::BreakpointType type, const debug_ipc::AddressRange& range) {
  if (!debug_ipc::IsWatchpointType(type))
    return std::nullopt;

  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return std::nullopt;

  DEBUG_LOG(Thread) << "Before installing watchpoint for range " << range.ToString() << std::endl
                    << regs->ToString();

  auto result = regs->SetWatchpoint(type, range, arch::GetHardwareWatchpointCount());
  if (!result)
    return std::nullopt;

  DEBUG_LOG(Thread) << "After installing watchpoint: " << std::endl << regs->ToString();

  if (!SetDebugRegisters(*regs))
    return std::nullopt;
  return result;
}

bool ZirconThreadHandle::UninstallWatchpoint(const debug_ipc::AddressRange& range) {
  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return false;

  DEBUG_LOG(Thread) << "Before uninstalling watchpoint: " << std::endl << regs->ToString();

  // x64 doesn't support ranges.
  if (!regs->RemoveWatchpoint(range, arch::GetHardwareWatchpointCount()))
    return false;

  DEBUG_LOG(Thread) << "After uninstalling watchpoint: " << std::endl << regs->ToString();
  return SetDebugRegisters(*regs);
}

}  // namespace debug_agent
