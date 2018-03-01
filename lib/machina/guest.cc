// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/machina/guest.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <zircon/device/sysinfo.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/hypervisor.h>
#include <zircon/syscalls/port.h>
#include <zircon/threads.h>

#include "garnet/lib/machina/io.h"
#include "lib/fxl/logging.h"

static constexpr char kResourcePath[] = "/dev/misc/sysinfo";

// Number of threads reading from the async device port.
static constexpr size_t kNumAsyncWorkers = 1;

static zx_status_t guest_get_resource(zx_handle_t* resource) {
  int fd = open(kResourcePath, O_RDWR);
  if (fd < 0)
    return ZX_ERR_IO;
  ssize_t n = ioctl_sysinfo_get_hypervisor_resource(fd, resource);
  close(fd);
  return n < 0 ? ZX_ERR_IO : ZX_OK;
}

static constexpr uint32_t trap_kind(machina::TrapType type) {
  switch (type) {
    case machina::TrapType::MMIO_SYNC:
      return ZX_GUEST_TRAP_MEM;
    case machina::TrapType::MMIO_BELL:
      return ZX_GUEST_TRAP_BELL;
    case machina::TrapType::PIO_SYNC:
      return ZX_GUEST_TRAP_IO;
    default:
      ZX_PANIC("Unhandled TrapType %d\n", static_cast<int>(type));
      return 0;
  }
}

static constexpr zx_handle_t get_trap_port(machina::TrapType type,
                                           zx_handle_t port) {
  switch (type) {
    case machina::TrapType::MMIO_BELL:
      return port;
    case machina::TrapType::PIO_SYNC:
    case machina::TrapType::MMIO_SYNC:
      return ZX_HANDLE_INVALID;
    default:
      ZX_PANIC("Unhandled TrapType %d\n", static_cast<int>(type));
      return ZX_HANDLE_INVALID;
  }
}

namespace machina {

zx_status_t Guest::Init(size_t mem_size) {
  zx_status_t status = phys_mem_.Init(mem_size);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create guest physical memory";
    return status;
  }

  zx_handle_t resource;
  status = guest_get_resource(&resource);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to get hypervisor resource";
    return status;
  }

  status = zx_guest_create(resource, 0, phys_mem_.vmo().get(), &guest_);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create guest";
    return status;
  }
  zx_handle_close(resource);

  status = zx::port::create(0, &port_);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create port";
    return status;
  }

  for (size_t i = 0; i < kNumAsyncWorkers; ++i) {
    thrd_t thread;
    auto thread_func =
        +[](void* arg) { return static_cast<Guest*>(arg)->IoThread(); };
    int ret = thrd_create_with_name(&thread, thread_func, this, "io-handler");
    if (ret != thrd_success) {
      FXL_LOG(ERROR) << "Failed to create io handler thread " << ret;
      return ZX_ERR_INTERNAL;
    }

    ret = thrd_detach(thread);
    if (ret != thrd_success) {
      FXL_LOG(ERROR) << "Failed to detach io handler thread " << ret;
      return ZX_ERR_INTERNAL;
    }
  }

  return ZX_OK;
}

Guest::~Guest() {
  zx_handle_close(guest_);
}

zx_status_t Guest::IoThread() {
  while (true) {
    zx_port_packet_t packet;
    zx_status_t status = port_.wait(zx::time::infinite(), &packet, 0);
    if (status != ZX_OK) {
      FXL_LOG(ERROR) << "Failed to wait for device port " << status;
      break;
    }
    if (packet.type != ZX_PKT_TYPE_GUEST_BELL) {
      FXL_LOG(ERROR) << "Unsupported async packet type " << packet.type;
      return ZX_ERR_NOT_SUPPORTED;
    }

    IoValue value = {};
    uint64_t addr = packet.guest_bell.addr;
    status = trap_key_to_mapping(packet.key)->Write(addr, value);
    if (status != ZX_OK) {
      FXL_LOG(ERROR) << "Unable to handle packet for device " << status;
      break;
    }
  }

  return ZX_ERR_INTERNAL;
}

zx_status_t Guest::CreateMapping(TrapType type,
                                 uint64_t addr,
                                 size_t size,
                                 uint64_t offset,
                                 IoHandler* handler) {
  fbl::AllocChecker ac;
  auto mapping =
      fbl::make_unique_checked<IoMapping>(&ac, addr, size, offset, handler);
  if (!ac.check())
    return ZX_ERR_NO_MEMORY;

  // Set a trap for the IO region. We set the 'key' to be the address of the
  // mapping so that we get the pointer to the mapping provided to us in port
  // packets.
  zx_handle_t port = get_trap_port(type, port_.get());
  uint32_t kind = trap_kind(type);
  uint64_t key = reinterpret_cast<uintptr_t>(mapping.get());
  zx_status_t status = zx_guest_set_trap(guest_, kind, addr, size, port, key);
  if (status != ZX_OK)
    return status;

  mappings_.push_front(fbl::move(mapping));
  return ZX_OK;
}

void Guest::RegisterVcpuFactory(VcpuFactory factory) {
  vcpu_factory_ = fbl::move(factory);
}

zx_status_t Guest::StartVcpu(uintptr_t entry, uint64_t id) {
  fbl::AutoLock lock(&mutex_);
  if (id >= kMaxVcpus) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (vcpus_[0] == nullptr && id != 0) {
    FXL_LOG(ERROR) << "VCPU-0 must be started before other VCPUs";
    return ZX_ERR_BAD_STATE;
  }
  if (vcpus_[id] != nullptr) {
    // The guest might make multiple requests to start a particular VCPU. On
    // x86, the guest should send two START_UP IPIs but we initialise the VCPU
    // on the first. So, we ignore subsequent requests.
    return ZX_OK;
  }
  auto vcpu = fbl::make_unique<Vcpu>();
  zx_status_t status = vcpu_factory_(this, entry, id, vcpu.get());
  if (status != ZX_OK) {
    return status;
  }
  vcpus_[id] = fbl::move(vcpu);

  return ZX_OK;
}

zx_status_t Guest::SignalInterrupt(uint32_t mask, uint8_t vector) {
  for (size_t id = 0; id != kMaxVcpus; ++id) {
    if (vcpus_[id] == nullptr || !((1u << id) & mask)) {
      continue;
    }
    zx_status_t status = vcpus_[id]->Interrupt(vector);
    if (status != ZX_OK) {
      return status;
    }
  }
  return ZX_OK;
}

zx_status_t Guest::Join() {
  // We assume that the VCPU-0 thread will be started first, and that no
  // additional VCPUs will be brought up after it terminates.
  zx_status_t status = vcpus_[0]->Join();

  // Once the initial VCPU has terminated, wait for any additional VCPUs.
  for (size_t id = 1; id != kMaxVcpus; ++id) {
    if (vcpus_[id] != nullptr) {
      zx_status_t vcpu_status = vcpus_[id]->Join();
      if (vcpu_status != ZX_OK) {
        status = vcpu_status;
      }
    }
  }

  return status;
}

}  // namespace machina
