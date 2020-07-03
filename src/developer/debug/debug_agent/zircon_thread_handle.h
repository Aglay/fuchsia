// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ZIRCON_THREAD_HANDLE_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ZIRCON_THREAD_HANDLE_H_

#include <lib/zx/thread.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/thread_handle.h"

namespace debug_agent {

class ZirconThreadHandle final : public ThreadHandle {
 public:
  // The process koid and thread koid are provided in the constructor since these are normally known
  // in advance and they never change.
  ZirconThreadHandle(std::shared_ptr<arch::ArchProvider> arch_provider, zx_koid_t process_koid,
                     zx_koid_t thread_koid, zx::thread t);

  // ThreadHandle implementation.
  const zx::thread& GetNativeHandle() const override { return thread_; }
  zx::thread& GetNativeHandle() override { return thread_; }
  zx_koid_t GetKoid() const override { return thread_koid_; }
  uint32_t GetState() const override;
  debug_ipc::ThreadRecord GetThreadRecord() const override;
  zx::suspend_token Suspend() override;
  std::vector<debug_ipc::Register> ReadRegisters(
      const std::vector<debug_ipc::RegisterCategory>& cats_to_get) const override;
  std::vector<debug_ipc::Register> WriteRegisters(
      const std::vector<debug_ipc::Register>& regs) override;
  zx_status_t InstallHWBreakpoint(uint64_t address) override;
  zx_status_t UninstallHWBreakpoint(uint64_t address) override;
  arch::WatchpointInstallationResult InstallWatchpoint(
      debug_ipc::BreakpointType type, const debug_ipc::AddressRange& range) override;
  zx_status_t UninstallWatchpoint(const debug_ipc::AddressRange& range) override;

 private:
  zx_status_t ReadDebugRegisters(zx_thread_state_debug_regs* regs) const;
  zx_status_t WriteDebugRegisters(const zx_thread_state_debug_regs& regs);

  std::shared_ptr<arch::ArchProvider> arch_provider_;

  zx_koid_t process_koid_;
  zx_koid_t thread_koid_;
  zx::thread thread_;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ZIRCON_THREAD_HANDLE_H_
