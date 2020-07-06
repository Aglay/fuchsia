// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_PROCESS_HANDLE_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_PROCESS_HANDLE_H_

#include <lib/zx/process.h>
#include <zircon/status.h>

#include <vector>

namespace debug_ipc {
struct AddressRegion;
struct MemoryBlock;
}  // namespace debug_ipc

namespace debug_agent {

class ProcessHandle {
 public:
  virtual ~ProcessHandle() = default;

  // Access to the underlying native process object. This is for porting purposes, ideally this
  // object would encapsulate all details about the process for testing purposes and this getter
  // would be removed. In testing situations, the returned value may be an empty object,
  // TODO(brettw) Remove this.
  virtual const zx::process& GetNativeHandle() const = 0;
  virtual zx::process& GetNativeHandle() = 0;

  virtual zx_koid_t GetKoid() const = 0;

  virtual zx_status_t GetInfo(zx_info_process* process) const = 0;

  // Returns the address space information. If the address is non-null, only the regions covering
  // that address will be returned. Otherwise all regions will be returned.
  virtual std::vector<debug_ipc::AddressRegion> GetAddressSpace(uint64_t address) const = 0;

  virtual zx_status_t ReadMemory(uintptr_t address, void* buffer, size_t len,
                                 size_t* actual) const = 0;
  virtual zx_status_t WriteMemory(uintptr_t address, const void* buffer, size_t len,
                                  size_t* actual) = 0;

  // Does a mapped-memory-aware read of the process memory. The result can contain holes which
  // the normal ReadMemory call above can't handle. On failure, there will be one block returned
  // covering the requested size, marked invalid.
  virtual std::vector<debug_ipc::MemoryBlock> ReadMemoryBlocks(uint64_t address,
                                                               uint32_t size) const = 0;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_PROCESS_HANDLE_H_
