// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_MACHINA_ARCH_ARM64_GIC_DISTRIBUTOR_H_
#define GARNET_LIB_MACHINA_ARCH_ARM64_GIC_DISTRIBUTOR_H_

#include <fuchsia/sysinfo/cpp/fidl.h>
#include <limits.h>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "garnet/lib/machina/io.h"
#include "garnet/lib/machina/platform_device.h"

// NOTE(abdulla): Ideally, this should be guest_config.h.
struct InterruptSpec {
  uint32_t vector;
  uint32_t options;
};

namespace machina {

class Guest;

// Implements GIC redistributor.
class GicRedistributor : public IoHandler {
 public:
  GicRedistributor(uint16_t id, bool last) : id_(id), last_(last) {}

  zx_status_t Read(uint64_t addr, IoValue* value) const override;
  zx_status_t Write(uint64_t addr, const IoValue& value) override;

  bool IsEnabled(uint32_t vector) const;

 private:
  uint16_t id_;
  bool last_;

  // Tracks whether SGIs and PPIs are enabled.
  uint32_t enabled_;
};

// Implements GIC distributor.
class GicDistributor : public IoHandler, public PlatformDevice {
 public:
  GicDistributor(Guest* guest);

  zx_status_t Init(uint8_t num_cpus,
                   const std::vector<InterruptSpec>& interrupts)
      __TA_NO_THREAD_SAFETY_ANALYSIS;

  zx_status_t Interrupt(uint32_t vector);

  // |IoHandler|
  zx_status_t Read(uint64_t addr, IoValue* value) const override;
  zx_status_t Write(uint64_t addr, const IoValue& value) override;

  // |PlatformDevice|
  zx_status_t ConfigureZbi(void* zbi_base, size_t zbi_max) const override;
  zx_status_t ConfigureDtb(void* dtb) const override;

 private:
  // NOTE: This must match the same constant in arch/hypervisor.h within Zircon.
  static constexpr size_t kNumInterrupts = 256;
  static constexpr uint8_t kSpiBase = 32;

  Guest* guest_;
  fuchsia::sysinfo::InterruptControllerType type_ =
      fuchsia::sysinfo::InterruptControllerType::GIC_V2;
  std::unordered_map<uint32_t, zx::interrupt> interrupts_;

  mutable std::mutex mutex_;
  bool affinity_routing_ __TA_GUARDED(mutex_) = false;
  std::vector<GicRedistributor> __TA_GUARDED(mutex_) redistributors_;

  // Tracks whether SPIs are enabled.
  uint8_t enabled_[(kNumInterrupts - kSpiBase) / CHAR_BIT] __TA_GUARDED(
      mutex_) = {};

  // SPI routing uses these CPU masks.
  uint8_t cpu_masks_[kNumInterrupts - kSpiBase] __TA_GUARDED(mutex_) = {};

  zx_status_t TargetInterrupt(uint32_t vector, uint8_t cpu_mask);
  zx_status_t BindVcpus(uint32_t vector, uint8_t cpu_mask);
};

}  // namespace machina

#endif  // GARNET_LIB_MACHINA_ARCH_ARM64_GIC_DISTRIBUTOR_H_
