// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_HWSTRESS_FLASH_STRESS_H_
#define GARNET_BIN_HWSTRESS_FLASH_STRESS_H_

#include <lib/zx/time.h>

#include <string>

#include <src/lib/uuid/uuid.h>

#include "args.h"
#include "status.h"

namespace hwstress {

// The GPT partition type used for partitions created by the flash test.
constexpr uuid::Uuid kTestPartGUID = uuid::Uuid({0xC6, 0x24, 0xF5, 0xDD, 0x9D, 0x88, 0x4C, 0x81,
                                                 0x99, 0x87, 0xCA, 0x92, 0xD1, 0x1B, 0x28, 0x89});

// Creates and manages the lifetime of a new partition backed by a
// Fuchsia Volume Manager instance.
class TemporaryFvmPartition {
 public:
  ~TemporaryFvmPartition();

  // Create a new partition.
  //
  // |fvm_path| should be the path to an FVM instance, such as
  // "/dev/sys/pci/00:00.0/ahci/sata0/block/fvm".
  //
  // |bytes_requested| is the maximum number of bytes callers will be able to use on the partition.
  // The returned partition may have greater than the requested number of bytes due to rounding and
  // overheads, or it may have less as space is lazily allocated by FVM, so the requested number of
  // bytes may not actually be available.
  //
  // Returns nullptr on failure.
  static std::unique_ptr<TemporaryFvmPartition> Create(int fvm_fd, uint64_t slices_requested);

  // Get the path to the created partition.
  std::string GetPartitionPath();

 private:
  std::string partition_path_;
  uuid::Uuid unique_guid_;

  TemporaryFvmPartition(std::string partition_path, uuid::Uuid unique_guid);
};

// Start a stress test.
bool StressFlash(StatusLine* status, const CommandLineArgs& args, zx::duration duration);

// Delete any persistent flash test partitions
void DestroyFlashTestPartitions(StatusLine* status);

}  // namespace hwstress

#endif  // GARNET_BIN_HWSTRESS_FLASH_STRESS_H_
