// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_H_

#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/exception.h>

#include "src/developer/debug/debug_agent/arch_helpers.h"
#include "src/developer/debug/debug_agent/arch_types.h"
#include "src/developer/debug/ipc/protocol.h"

namespace debug_agent {

class DebuggedThread;
class ThreadHandle;

namespace arch {

// This file contains architecture-specific low-level helper functions. It is like zircon_utils but
// the functions will have different implementations depending on CPU architecture.
//
// The functions here should be very low-level and are designed for the real (*_zircon.cc)
// implementations of the the various primitives. Cross-platform code should use interfaces like
// ThreadHandle for anything that might need mocking out.

// Our canonical breakpoint instruction for the current architecture. This is what we'll write for
// software breakpoints. Some platforms may have alternate encodings for software breakpoints, so to
// check if something is a breakpoint instruction, use arch::IsBreakpointInstruction() rather than
// checking for equality with this value.
extern const BreakInstructionType kBreakInstruction;

debug_ipc::Arch GetCurrentArch();

// Returns the number of hardware breakpoints and watchpoints on the current system.
uint32_t GetHardwareBreakpointCount();
uint32_t GetHardwareWatchpointCount();

// Converts the given register structure to a vector of debug_ipc registers.
void SaveGeneralRegs(const zx_thread_state_general_regs& input,
                     std::vector<debug_ipc::Register>& out);

// The registers in the given category are appended to the given output vector.
zx_status_t ReadRegisters(const zx::thread& thread, const debug_ipc::RegisterCategory& cat,
                          std::vector<debug_ipc::Register>& out);

// The registers must all be in the same category.
zx_status_t WriteRegisters(zx::thread& thread, const debug_ipc::RegisterCategory& cat,
                           const std::vector<debug_ipc::Register>& registers);

// Converts a Zircon exception type to a debug_ipc one. Some exception types require querying the
// thread's debug registers. If needed, the given thread will be used for that.
debug_ipc::ExceptionType DecodeExceptionType(const zx::thread& thread, uint32_t exception_type);

// Converts an architecture-specific exception record to a cross-platform one.
debug_ipc::ExceptionRecord FillExceptionRecord(const zx_exception_report_t& in);

// Returns the address of the breakpoint instruction given the address of a software breakpoint
// exception.
uint64_t BreakpointInstructionForSoftwareExceptionAddress(uint64_t exception_addr);

// Returns the instruction following the one causing the given software exception.
uint64_t NextInstructionForSoftwareExceptionAddress(uint64_t exception_addr);

// Returns true if the given opcode is a breakpoint instruction. This checked for equality with
// kBreakInstruction and also checks other possible breakpoint encodings for the current platform.
bool IsBreakpointInstruction(BreakInstructionType instruction);

// Returns the address of the instruction that hit the exception from the address reported by the
// exception.
uint64_t BreakpointInstructionForHardwareExceptionAddress(uint64_t exception_addr);

}  // namespace arch
}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_H_
