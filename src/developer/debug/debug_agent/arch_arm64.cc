// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/macros.h>
#include <zircon/hw/debug/arm64.h>
#include <zircon/status.h>
#include <zircon/syscalls/exception.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/arch_helpers.h"
#include "src/developer/debug/debug_agent/arch_types.h"
#include "src/developer/debug/debug_agent/debugged_thread.h"
#include "src/developer/debug/ipc/decode_exception.h"
#include "src/developer/debug/ipc/register_desc.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/strings/string_printf.h"

// Notes on ARM64 architecture:
//
// Information was obtained from the Arm® Architecture Reference Manual Armv8, for Armv8-A
// architecture profile:
//
// https://developer.arm.com/docs/ddi0487/latest/arm-architecture-reference-manual-armv8-for-armv8-a-architecture-profile
//
// In order to obtain information about the registers below, the easiest way to do it is to do
// search (ctrl-f) in the browser and the hit will probably will a link that you can press into
// the corresponding definition (eg. search for "dbgwcr" and then click on the link).
//
// Hardware Breakpoints
// -------------------------------------------------------------------------------------------------
//
// Hardware breakpoints permits to stop a thread when it accesses an address setup in one of the
// hw breakpoints registers. They will work independent whether the address in question is
// read-only or not.
// ARMv8 assures at least 2 hardware breakpoints.
//
// See zircon/system/public/hw/debug/arm64.h for more detailed information.
//
// DBGBVR<n>: Watchpoint Value Register.
//
// This register defines the value of the hw breakpoint <n> within the system. How that value is
// interpreted depends on the correspondent value of DBGBCR<n>.

// DBGBCR<n>: Debug Control Register for HW Breakpoint #n.
//
// Control register for HW breakpoints. There is one for each HW breakpoint present within the
// system. They go numbering by DBGBCR0, DBGBCR1, ... until the value defined in ID_AADFR0_EL1.
//
// For each control register, there is an equivalent DBGBVR<n> that holds the address the thread
// will compare against.
//
// The only register that needs to be set by the user is E (Bit 1). The other configuration is
// opaque and is handled by the kernel.
// See zircon/system/public/hw/debug/arm64.h for more detailed information.
//
// Watchpoints
// -------------------------------------------------------------------------------------------------
//
// Watchpoints permits to stop a thread when it read/writes to a particular address in memory.
// This will work even if the address is read-only memory (for a read, of course).
// ARMv8 assures at least 2 watchpoints.
//
// See zircon/system/public/hw/debug/arm64.h for more detailed information.
//
// DBGWVR<n>: Watchpoint Value Register.
//
// This register defines the value of the watchpoint <n> within the system. How that value is
// interpreted depends on the correspondent value of DBGWCR<n>.
//
// DBGWCR<n>: Watchpoint Control Register.
//
// Control register for watchpoints. There is one for each watchpoint present within the system.
// They go numbering by DBGWCR0, DBGWCR1, ... until the value defined ID_AAFR0_EL1.
// For each control register, there is an equivalent DBGWCR<n> that holds the address the thread
// will compare against. How this address is interpreted depends upon the configuration of the
// associated control register.
//
// The following are the bits that are most important,
//
// - E (Bit 1): Defines whether the watchpoint is enabled or not.
//
// - LSC (bits 3-4): Defines how the watchpoint works:
//                   01: Read from address.
//                   10: Write to address.
//                   11: Read/Write to address.
//
// - BAS (Bits 5-12): Defines which bytes are to be "matched" starting from the one defined in the
//                    value register. Each bit defines what bytes to match onto:
//
//                    0bxxxx'xxx1: Match DBGWVR<n> + 0
//                    0bxxxx'xx1x: Match DBGWVR<n> + 1
//                    0bxxxx'x1xx: Match DBGWVR<n> + 2
//                    0bxxxx'1xxx: Match DBGWVR<n> + 3
//                    0bxxx1'xxxx: Match DBGWVR<n> + 4
//                    0bxx1x'xxxx: Match DBGWVR<n> + 5
//                    0bx1xx'xxxx: Match DBGWVR<n> + 6
//                    0b1xxx'xxxx: Match DBGWVR<n> + 7
//
//                    These bits must be set contiguosly (there cannot be gaps between the first
//                    set bit and the last). Having DBGWVR not be 4-bytes aligned is deprecated.

namespace debug_agent {
namespace arch {

namespace {

using debug_ipc::RegisterID;

debug_ipc::Register CreateRegister(RegisterID id, uint32_t length, const void* val_ptr) {
  debug_ipc::Register reg;
  reg.id = id;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(val_ptr);
  reg.data.assign(ptr, ptr + length);
  return reg;
}

zx_status_t ReadGeneralRegs(const zx::thread& thread, std::vector<debug_ipc::Register>& out) {
  zx_thread_state_general_regs gen_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_GENERAL_REGS, &gen_regs, sizeof(gen_regs));
  if (status != ZX_OK)
    return status;

  arch::SaveGeneralRegs(gen_regs, out);
  return ZX_OK;
}

zx_status_t ReadVectorRegs(const zx::thread& thread, std::vector<debug_ipc::Register>& out) {
  zx_thread_state_vector_regs vec_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_VECTOR_REGS, &vec_regs, sizeof(vec_regs));
  if (status != ZX_OK)
    return status;

  out.push_back(CreateRegister(RegisterID::kARMv8_fpcr, 4u, &vec_regs.fpcr));
  out.push_back(CreateRegister(RegisterID::kARMv8_fpsr, 4u, &vec_regs.fpsr));

  auto base = static_cast<uint32_t>(RegisterID::kARMv8_v0);
  for (size_t i = 0; i < 32; i++) {
    auto reg_id = static_cast<RegisterID>(base + i);
    out.push_back(CreateRegister(reg_id, 16u, &vec_regs.v[i]));
  }

  return ZX_OK;
}

zx_status_t ReadDebugRegs(const zx::thread& thread, std::vector<debug_ipc::Register>& out) {
  zx_thread_state_debug_regs_t debug_regs;
  zx_status_t status =
      thread.read_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs, sizeof(debug_regs));
  if (status != ZX_OK)
    return status;

  if (debug_regs.hw_bps_count >= AARCH64_MAX_HW_BREAKPOINTS) {
    FX_LOGS(ERROR) << "Received too many HW breakpoints: " << debug_regs.hw_bps_count
                   << " (max: " << AARCH64_MAX_HW_BREAKPOINTS << ").";
    return ZX_ERR_INVALID_ARGS;
  }

  // HW breakpoints.
  {
    auto bcr_base = static_cast<uint32_t>(RegisterID::kARMv8_dbgbcr0_el1);
    auto bvr_base = static_cast<uint32_t>(RegisterID::kARMv8_dbgbvr0_el1);
    for (size_t i = 0; i < debug_regs.hw_bps_count; i++) {
      auto bcr_id = static_cast<RegisterID>(bcr_base + i);
      out.push_back(CreateRegister(bcr_id, sizeof(debug_regs.hw_bps[i].dbgbcr),
                                   &debug_regs.hw_bps[i].dbgbcr));

      auto bvr_id = static_cast<RegisterID>(bvr_base + i);
      out.push_back(CreateRegister(bvr_id, sizeof(debug_regs.hw_bps[i].dbgbvr),
                                   &debug_regs.hw_bps[i].dbgbvr));
    }
  }

  // Watchpoints.
  {
    auto bcr_base = static_cast<uint32_t>(RegisterID::kARMv8_dbgwcr0_el1);
    auto bvr_base = static_cast<uint32_t>(RegisterID::kARMv8_dbgwvr0_el1);
    for (size_t i = 0; i < debug_regs.hw_bps_count; i++) {
      auto bcr_id = static_cast<RegisterID>(bcr_base + i);
      out.push_back(CreateRegister(bcr_id, sizeof(debug_regs.hw_wps[i].dbgwcr),
                                   &debug_regs.hw_wps[i].dbgwcr));

      auto bvr_id = static_cast<RegisterID>(bvr_base + i);
      out.push_back(CreateRegister(bvr_id, sizeof(debug_regs.hw_wps[i].dbgwvr),
                                   &debug_regs.hw_wps[i].dbgwvr));
    }
  }

  // TODO(donosoc): Currently this registers that are platform information are
  //                being hacked out as HW breakpoint values in order to know
  //                what the actual settings are.
  //                This should be changed to get the actual values instead, but
  //                check in for now in order to continue.
  out.push_back(CreateRegister(RegisterID::kARMv8_id_aa64dfr0_el1, 8u,
                               &debug_regs.hw_bps[AARCH64_MAX_HW_BREAKPOINTS - 1].dbgbvr));
  out.push_back(CreateRegister(RegisterID::kARMv8_mdscr_el1, 8u,
                               &debug_regs.hw_bps[AARCH64_MAX_HW_BREAKPOINTS - 2].dbgbvr));

  return ZX_OK;
}

// Adapter class to allow the exception decoder to get the debug registers if needed.
class ExceptionInfo : public debug_ipc::Arm64ExceptionInfo {
 public:
  explicit ExceptionInfo(const zx::thread& thread) : thread_(thread) {}

  std::optional<uint32_t> FetchESR() const override {
    zx_thread_state_debug_regs_t debug_regs;
    zx_status_t status = thread_.read_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                                            sizeof(zx_thread_state_debug_regs_t));
    if (status != ZX_OK) {
      DEBUG_LOG(ArchArm64) << "Could not get ESR: " << zx_status_get_string(status);
      return std::nullopt;
    }

    return debug_regs.esr;
  }

 private:
  const zx::thread& thread_;
};

}  // namespace

// "BRK 0" instruction.
// - Low 5 bits = 0.
// - High 11 bits = 11010100001
// - In between 16 bits is the argument to the BRK instruction (in this case zero).
const BreakInstructionType kBreakInstruction = 0xd4200000;

::debug_ipc::Arch GetCurrentArch() { return ::debug_ipc::Arch::kArm64; }

void SaveGeneralRegs(const zx_thread_state_general_regs& input,
                     std::vector<debug_ipc::Register>& out) {
  // Add the X0-X29 registers.
  uint32_t base = static_cast<uint32_t>(RegisterID::kARMv8_x0);
  for (int i = 0; i < 30; i++) {
    RegisterID type = static_cast<RegisterID>(base + i);
    out.push_back(CreateRegister(type, 8u, &input.r[i]));
  }

  // Add the named ones.
  out.push_back(CreateRegister(RegisterID::kARMv8_lr, 8u, &input.lr));
  out.push_back(CreateRegister(RegisterID::kARMv8_sp, 8u, &input.sp));
  out.push_back(CreateRegister(RegisterID::kARMv8_pc, 8u, &input.pc));
  out.push_back(CreateRegister(RegisterID::kARMv8_cpsr, 8u, &input.cpsr));
  out.push_back(CreateRegister(RegisterID::kARMv8_tpidr, 8u, &input.tpidr));
}

zx_status_t ReadRegisters(const zx::thread& thread, const debug_ipc::RegisterCategory& cat,
                          std::vector<debug_ipc::Register>& out) {
  switch (cat) {
    case debug_ipc::RegisterCategory::kGeneral:
      return ReadGeneralRegs(thread, out);
    case debug_ipc::RegisterCategory::kFloatingPoint:
      // No FP registers
      return true;
    case debug_ipc::RegisterCategory::kVector:
      return ReadVectorRegs(thread, out);
    case debug_ipc::RegisterCategory::kDebug:
      return ReadDebugRegs(thread, out);
    default:
      FX_LOGS(ERROR) << "Invalid category: " << static_cast<uint32_t>(cat);
      return ZX_ERR_INVALID_ARGS;
  }
}

zx_status_t WriteRegisters(zx::thread& thread, const debug_ipc::RegisterCategory& category,
                           const std::vector<debug_ipc::Register>& registers) {
  switch (category) {
    case debug_ipc::RegisterCategory::kGeneral: {
      zx_thread_state_general_regs_t regs;
      zx_status_t res = thread.read_state(ZX_THREAD_STATE_GENERAL_REGS, &regs, sizeof(regs));
      if (res != ZX_OK)
        return res;

      // Overwrite the values.
      res = WriteGeneralRegisters(registers, &regs);
      if (res != ZX_OK)
        return res;

      return thread.write_state(ZX_THREAD_STATE_GENERAL_REGS, &regs, sizeof(regs));
    }
    case debug_ipc::RegisterCategory::kFloatingPoint: {
      return ZX_ERR_INVALID_ARGS;  // No floating point registers.
    }
    case debug_ipc::RegisterCategory::kVector: {
      zx_thread_state_vector_regs_t regs;
      zx_status_t res = thread.read_state(ZX_THREAD_STATE_VECTOR_REGS, &regs, sizeof(regs));
      if (res != ZX_OK)
        return res;

      // Overwrite the values.
      res = WriteVectorRegisters(registers, &regs);
      if (res != ZX_OK)
        return res;

      return thread.write_state(ZX_THREAD_STATE_VECTOR_REGS, &regs, sizeof(regs));
    }
    case debug_ipc::RegisterCategory::kDebug: {
      zx_thread_state_debug_regs_t regs;
      zx_status_t res = thread.read_state(ZX_THREAD_STATE_DEBUG_REGS, &regs, sizeof(regs));
      if (res != ZX_OK)
        return res;

      res = WriteDebugRegisters(registers, &regs);
      if (res != ZX_OK)
        return res;

      return thread.write_state(ZX_THREAD_STATE_DEBUG_REGS, &regs, sizeof(regs));
    }
    case debug_ipc::RegisterCategory::kNone:
    case debug_ipc::RegisterCategory::kLast:
      break;
  }
  FX_NOTREACHED();
  return ZX_ERR_INVALID_ARGS;
}

debug_ipc::ExceptionType DecodeExceptionType(const zx::thread& thread, uint32_t exception_type) {
  ExceptionInfo info(thread);
  return debug_ipc::DecodeException(exception_type, info);
}

debug_ipc::ExceptionRecord FillExceptionRecord(const zx_exception_report_t& in) {
  debug_ipc::ExceptionRecord record;

  record.valid = true;
  record.arch.arm64.esr = in.context.arch.u.arm_64.esr;
  record.arch.arm64.far = in.context.arch.u.arm_64.far;

  return record;
}

uint64_t BreakpointInstructionForSoftwareExceptionAddress(uint64_t exception_addr) {
  // ARM reports the exception for the exception instruction itself.
  return exception_addr;
}

uint64_t NextInstructionForSoftwareExceptionAddress(uint64_t exception_addr) {
  // For software exceptions, the exception address is the one that caused it,
  // so next one is just 4 bytes following.
  //
  // TODO(brettw) handle THUMB. When a software breakpoint is hit, ESR_EL1
  // will contain the "instruction length" field which for T32 instructions
  // will be 0 (indicating 16-bits). This exception state somehow needs to be
  // plumbed down to our exception handler.
  return exception_addr + 4;
}

bool IsBreakpointInstruction(BreakInstructionType instruction) {
  // The BRK instruction could have any number associated with it, even though we only write "BRK
  // 0", so check for the low 5 and high 11 bytes as described above.
  constexpr BreakInstructionType kMask = 0b11111111111000000000000000011111;
  return (instruction & kMask) == kBreakInstruction;
}

uint64_t BreakpointInstructionForHardwareExceptionAddress(uint64_t exception_addr) {
  // arm64 will return the address of the instruction *about* to be executed.
  return exception_addr;
}

}  // namespace arch
}  // namespace debug_agent
