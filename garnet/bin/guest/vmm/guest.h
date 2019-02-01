// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_VMM_GUEST_H_
#define GARNET_BIN_GUEST_VMM_GUEST_H_

#include <array>
#include <forward_list>
#include <shared_mutex>

#include <lib/async-loop/cpp/loop.h>
#include <zx/guest.h>
#include <zx/vmar.h>

#include "garnet/bin/guest/vmm/device/phys_mem.h"
#include "garnet/bin/guest/vmm/guest_config.h"
#include "garnet/bin/guest/vmm/io.h"
#include "garnet/bin/guest/vmm/vcpu.h"

enum class TrapType {
  MMIO_SYNC = 0,
  MMIO_BELL = 1,
  PIO_SYNC = 2,
};

class Guest {
 public:
  static constexpr size_t kMaxVcpus = 16u;
  using VcpuArray = std::array<std::unique_ptr<Vcpu>, kMaxVcpus>;
  using IoMappingList = std::forward_list<IoMapping>;

  zx_status_t Init(const std::vector<MemorySpec>& memory);

  const PhysMem& phys_mem() const { return phys_mem_; }
  const zx::guest& object() { return guest_; }
  async_dispatcher_t* device_dispatcher() const {
    return device_loop_.dispatcher();
  }

  // Setup a trap to delegate accesses to an IO region to |handler|.
  zx_status_t CreateMapping(TrapType type, uint64_t addr, size_t size,
                            uint64_t offset, IoHandler* handler);

  // Creates a VMAR for a specific region of guest memory.
  zx_status_t CreateSubVmar(uint64_t addr, size_t size, zx::vmar* vmar);

  // Starts a VCPU. The first VCPU must have an |id| of 0.
  zx_status_t StartVcpu(uint64_t id, zx_gpaddr_t entry, zx_gpaddr_t boot_ptr);

  // Signals an interrupt to the VCPUs indicated by |mask|.
  zx_status_t Interrupt(uint64_t mask, uint8_t vector);

  // Waits for all VCPUs associated with the guest to finish executing.
  zx_status_t Join();

  const IoMappingList& mappings() const { return mappings_; }
  const VcpuArray& vcpus() const { return vcpus_; }

 private:
  zx::guest guest_;
  zx::vmar vmar_;
  PhysMem phys_mem_;
  IoMappingList mappings_;

  std::shared_mutex mutex_;
  VcpuArray vcpus_;

  async::Loop device_loop_{&kAsyncLoopConfigNoAttachToThread};
};

#endif  // GARNET_BIN_GUEST_VMM_GUEST_H_
