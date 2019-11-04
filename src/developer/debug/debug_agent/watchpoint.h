// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_

#include <stdint.h>
#include <zircon/types.h>

#include <set>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/process_breakpoint.h"

namespace debug_agent {

class Watchpoint : public ProcessBreakpoint {
 public:
  explicit Watchpoint(Breakpoint* breakpoint, DebuggedProcess* process,
                      std::shared_ptr<arch::ArchProvider> arch_provider,
                      const debug_ipc::AddressRange& range);
  ~Watchpoint();

  debug_ipc::BreakpointType Type() const override { return debug_ipc::BreakpointType::kWatchpoint; }
  bool Installed(zx_koid_t thread_koid) const override;

  zx_status_t Update() override;

  const debug_ipc::AddressRange& range() const { return range_; }

  const std::set<zx_koid_t>& installed_threads() const { return installed_threads_; }

  // Public ProcessBreakpoint overrides. See ProcessBreakpoint for more details.
  void EndStepOver(DebuggedThread* thread) override;
  void ExecuteStepOver(DebuggedThread* thread) override;

  void StepOverCleanup(DebuggedThread* thread) override {}

 private:
  zx_status_t Install(DebuggedThread* thread);

  zx_status_t Uninstall(DebuggedThread* thread) override;
  zx_status_t Uninstall() override;

  debug_ipc::AddressRange range_;

  std::shared_ptr<arch::ArchProvider> arch_provider_;

  std::set<zx_koid_t> installed_threads_;
  std::set<zx_koid_t> current_stepping_over_threads_;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_
