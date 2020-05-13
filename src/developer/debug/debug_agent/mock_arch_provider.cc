// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/mock_arch_provider.h"

#include <lib/syslog/cpp/macros.h>

namespace debug_agent {

zx_status_t MockArchProvider::ReadGeneralState(const zx::thread& handle,
                                               zx_thread_state_general_regs* regs) const {
  // Not implemented by this mock.
  FX_NOTREACHED();
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t MockArchProvider::WriteGeneralState(const zx::thread& handle,
                                                const zx_thread_state_general_regs& regs) {
  // Not implemented by this mock.
  FX_NOTREACHED();
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t MockArchProvider::ReadDebugState(const zx::thread& handle,
                                             zx_thread_state_debug_regs* regs) const {
  // Not implemented by this mock.
  FX_NOTREACHED();
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t MockArchProvider::WriteDebugState(const zx::thread& handle,
                                              const zx_thread_state_debug_regs& regs) {
  // Not implemented by this mock.
  FX_NOTREACHED();
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t MockArchProvider::WriteSingleStep(const zx::thread& thread, bool single_step) {
  // Not implemented by this mock.
  FX_NOTREACHED();
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t MockArchProvider::GetInfo(const zx::thread& thread, zx_object_info_topic_t topic,
                                      void* buffer, size_t buffer_size, size_t* actual = nullptr,
                                      size_t* avail = nullptr) const {
  // TODO this should be mocked instead. But currently there's no way to mock the thread passed as
  // input so there's no point in mocking the results from this.
  return thread.get_info(topic, buffer, buffer_size, actual, avail);
}

void MockArchProvider::FillExceptionRecord(const zx::thread&,
                                           debug_ipc::ExceptionRecord* out) const {
  out->valid = false;
}

zx_status_t MockArchProvider::InstallHWBreakpoint(const zx::thread& thread, uint64_t address) {
  bp_installs_[address]++;
  return ZX_OK;
}

zx_status_t MockArchProvider::UninstallHWBreakpoint(const zx::thread& thread, uint64_t address) {
  bp_uninstalls_[address]++;
  return ZX_OK;
}

arch::WatchpointInstallationResult MockArchProvider::InstallWatchpoint(
    debug_ipc::BreakpointType, const zx::thread&, const debug_ipc::AddressRange& range) {
  wp_installs_[range]++;
  return arch::WatchpointInstallationResult(ZX_OK, range, 0);
}

zx_status_t MockArchProvider::UninstallWatchpoint(const zx::thread&,
                                                  const debug_ipc::AddressRange& range) {
  wp_uninstalls_[range]++;
  return ZX_OK;
}

size_t MockArchProvider::BreakpointInstallCount(uint64_t address) const {
  auto it = bp_installs_.find(address);
  if (it == bp_installs_.end())
    return 0;
  return it->second;
}

size_t MockArchProvider::TotalBreakpointInstallCalls() const {
  int total = 0;
  for (auto it : bp_installs_) {
    total += it.second;
  }
  return total;
}

size_t MockArchProvider::BreakpointUninstallCount(uint64_t address) const {
  auto it = bp_uninstalls_.find(address);
  if (it == bp_uninstalls_.end())
    return 0;
  return it->second;
}

size_t MockArchProvider::TotalBreakpointUninstallCalls() const {
  int total = 0;
  for (auto it : bp_uninstalls_) {
    total += it.second;
  }
  return total;
}

size_t MockArchProvider::WatchpointInstallCount(const debug_ipc::AddressRange& range) const {
  auto it = wp_installs_.find(range);
  if (it == wp_installs_.end())
    return 0;
  return it->second;
}

size_t MockArchProvider::TotalWatchpointInstallCalls() const {
  int total = 0;
  for (auto it : wp_installs_) {
    total += it.second;
  }
  return total;
}

size_t MockArchProvider::WatchpointUninstallCount(const debug_ipc::AddressRange& range) const {
  auto it = wp_uninstalls_.find(range);
  if (it == wp_uninstalls_.end())
    return 0;
  return it->second;
}

size_t MockArchProvider::TotalWatchpointUninstallCalls() const {
  int total = 0;
  for (auto it : wp_uninstalls_) {
    total += it.second;
  }
  return total;
}

}  // namespace debug_agent
