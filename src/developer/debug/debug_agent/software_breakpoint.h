// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_SOFTWARE_BREAKPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_SOFTWARE_BREAKPOINT_H_

#include <zircon/status.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/process_breakpoint.h"
#include "src/developer/debug/ipc/protocol.h"

namespace debug_agent {

class ProcessMemoryAccessor;

class SoftwareBreakpoint : public ProcessBreakpoint {
 public:
  explicit SoftwareBreakpoint(Breakpoint* breakpoint, DebuggedProcess* process,
                              ProcessMemoryAccessor* memory_accessor, uint64_t address);

  SoftwareBreakpoint(ProcessMemoryAccessor*);
  ~SoftwareBreakpoint();

  debug_ipc::BreakpointType Type() const override { return debug_ipc::BreakpointType::kSoftware; }

  bool Installed() const override { return installed_; }

  void FixupMemoryBlock(debug_ipc::MemoryBlock* block) override;

 private:
  // ProcessBreakpoint overrides.
  zx_status_t Update() override;
  zx_status_t Install() override;
  void Uninstall() override;

  ProcessMemoryAccessor* memory_accessor_;  // Not-owning.

  // Set to true when the instruction has been replaced.
  bool installed_ = false;

  // Previous memory contents before being replaced with the break instruction.
  arch::BreakInstructionType previous_data_ = 0;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_SOFTWARE_BREAKPOINT_H_
