// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/debug_agent/arch.h"

#include <zircon/syscalls/exception.h>

#include "garnet/lib/debug_ipc/register_desc.h"
#include "garnet/public/lib/fxl/strings/string_printf.h"
#include "lib/fxl/logging.h"

namespace debug_agent {
namespace arch {

// "BRK 0" instruction.
// - Low 5 bits = 0.
// - High 11 bits = 11010100001
// - In between 16 bits is the argument to the BRK instruction (in this case
//   zero).
const BreakInstructionType kBreakInstruction = 0xd4200000;

uint64_t ArchProvider::BreakpointInstructionForSoftwareExceptionAddress(
    uint64_t exception_addr) {
  // ARM reports the exception for the exception instruction itself.
  return exception_addr;
}

uint64_t ArchProvider::NextInstructionForSoftwareExceptionAddress(
    uint64_t exception_addr) {
  // For software exceptions, the exception address is the one that caused it,
  // so next one is just 4 bytes following.
  //
  // TODO(brettw) handle THUMB. When a software breakpoint is hit, ESR_EL1
  // will contain the "instruction length" field which for T32 instructions
  // will be 0 (indicating 16-bits). This exception state somehow needs to be
  // plumbed down to our exception handler.
  return exception_addr + 4;
}

bool ArchProvider::IsBreakpointInstruction(zx::process& process,
                                           uint64_t address) {
  BreakInstructionType data;
  size_t actual_read = 0;
  if (process.read_memory(address, &data, sizeof(BreakInstructionType),
                          &actual_read) != ZX_OK ||
      actual_read != sizeof(BreakInstructionType))
    return false;

  // The BRK instruction could have any number associated with it, even though
  // we only write "BRK 0", so check for the low 5 and high 11 bytes as
  // described above.
  constexpr BreakInstructionType kMask = 0b11111111111000000000000000011111;
  return (data & kMask) == kBreakInstruction;
}

uint64_t* ArchProvider::IPInRegs(zx_thread_state_general_regs* regs) {
  return &regs->pc;
}
uint64_t* ArchProvider::SPInRegs(zx_thread_state_general_regs* regs) {
  return &regs->sp;
}
uint64_t* ArchProvider::BPInRegs(zx_thread_state_general_regs* regs) {
  return &regs->r[29];
}

::debug_ipc::Arch ArchProvider::GetArch() { return ::debug_ipc::Arch::kArm64; }

namespace {

using debug_ipc::RegisterID;

inline debug_ipc::Register CreateRegister(RegisterID id, uint32_t length,
                                          const void* val_ptr) {
  debug_ipc::Register reg;
  reg.id = id;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(val_ptr);
  reg.data.assign(ptr, ptr + length);
  return reg;
}

inline bool ReadGeneralRegs(const zx::thread& thread,
                                   std::vector<debug_ipc::Register>* out) {
  // We get the general state registers.
  zx_thread_state_general_regs gen_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_GENERAL_REGS,
                                         &gen_regs, sizeof(gen_regs));
  if (status != ZX_OK)
    return false;

  // We add the X0-X29 registers.
  uint32_t base = static_cast<uint32_t>(RegisterID::kARMv8_x0);
  for (int i = 0; i < 30; i++) {
    RegisterID type = static_cast<RegisterID>(base + i);
    out->push_back(CreateRegister(type, 8u, &gen_regs.r[i]));
  }

  // Add the named out.
  out->push_back(CreateRegister(RegisterID::kARMv8_lr, 8u, &gen_regs.lr));
  out->push_back(CreateRegister(RegisterID::kARMv8_sp, 8u, &gen_regs.sp));
  out->push_back(CreateRegister(RegisterID::kARMv8_pc, 8u, &gen_regs.pc));
  out->push_back(CreateRegister(RegisterID::kARMv8_cpsr, 8u, &gen_regs.cpsr));

  return true;
}

inline bool ReadVectorRegs(const zx::thread& thread,
                                  std::vector<debug_ipc::Register>* out) {
  zx_thread_state_vector_regs vec_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_VECTOR_REGS, &vec_regs,
                                         sizeof(vec_regs));
  if (status != ZX_OK)
    return false;

  out->push_back(CreateRegister(RegisterID::kARMv8_fpcr, 4u, &vec_regs.fpcr));
  out->push_back(CreateRegister(RegisterID::kARMv8_fpsr, 4u, &vec_regs.fpsr));

  auto base = static_cast<uint32_t>(RegisterID::kARMv8_v0);
  for (size_t i = 0; i < 32; i++) {
    auto reg_id = static_cast<RegisterID>(base + i);
    out->push_back(CreateRegister(reg_id, 16u, &vec_regs.v[i]));
  }

  return true;
}

}  // namespace

bool ArchProvider::GetRegisters(const debug_ipc::RegisterCategory::Type& cat,
                                const zx::thread& thread,
                                std::vector<debug_ipc::Register>* out) {
  switch (cat) {
    case debug_ipc::RegisterCategory::Type::kGeneral:
      return ReadGeneralRegs(thread, out);
    case debug_ipc::RegisterCategory::Type::kFloatingPoint:
      // No FP registers
      return true;
    case debug_ipc::RegisterCategory::Type::kVector:
      return ReadVectorRegs(thread, out);
    case debug_ipc::RegisterCategory::Type::kDebug:
      // TODO(donosoc): Read ARM64 debug registers.
      return false;
    default:
      FXL_LOG(ERROR) << "Invalid category: " << static_cast<uint32_t>(cat);
      return false;
  }
}

debug_ipc::NotifyException::Type HardwareNotificationType(const zx::thread&) {
  // TODO: For now zxdb only supports single step.
  return debug_ipc::NotifyException::Type::kSingleStep;
}

debug_ipc::NotifyException::Type ArchProvider::DecodeExceptionType(
    const DebuggedThread& thread, uint32_t exception_type) {
  switch (exception_type) {
    case ZX_EXCP_SW_BREAKPOINT:
      return debug_ipc::NotifyException::Type::kSoftware;
    case ZX_EXCP_HW_BREAKPOINT:
      // For now HW exception means single step.
      return debug_ipc::NotifyException::Type::kSingleStep;
    default:
      return debug_ipc::NotifyException::Type::kGeneral;
  }

  FXL_NOTREACHED();
  return debug_ipc::NotifyException::Type::kLast;
}

// HW Breakpoints --------------------------------------------------------------

uint64_t ArchProvider::BreakpointInstructionForHardwareExceptionAddress(
    uint64_t exception_addr) {
  FXL_NOTREACHED() << "NOT IMPLEMENTED";
  return exception_addr;
}

uint64_t ArchProvider::NextInstructionForHardwareExceptionAddress(
    uint64_t exception_addr) {
  FXL_NOTREACHED() << "NOT IMPLEMENTED";
  return exception_addr;
}

debug_ipc::NotifyException::Type HardwareNotificationType(
    const DebuggedThread& thread) {
  // TODO(donosoc): Implement hw exception detection logic.
    return debug_ipc::NotifyException::Type::kSingleStep;
}


zx_status_t ArchProvider::InstallHWBreakpoint(zx::thread*, uint64_t address) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t ArchProvider::UninstallHWBreakpoint(zx::thread*, uint64_t address) {
  return ZX_ERR_NOT_SUPPORTED;
}

}  // namespace arch
}  // namespace debug_agent
