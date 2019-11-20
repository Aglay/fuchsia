// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <threads.h>

#include <array>
#include <atomic>

#include <ddk/trace/event.h>
#include <ddktl/device.h>
#include <ddktl/protocol/block.h>
#include <ddktl/protocol/block/partition.h>
#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <lib/operation/block.h>
#include <lib/zircon-internal/thread_annotations.h>

#include "sdmmc-device.h"

namespace sdmmc {

// See the eMMC specification section 7.4.69 for these constants.
enum EmmcPartition : uint8_t {
  USER_DATA_PARTITION = 0x0,
  BOOT_PARTITION_1 = 0x1,
  BOOT_PARTITION_2 = 0x2,
  PARTITION_COUNT,
};

using BlockOperation = block::BorrowedOperation<EmmcPartition>;

class SdmmcBlockDevice;
class PartitionDevice;

using PartitionDeviceType =
    ddk::Device<PartitionDevice, ddk::GetSizable, ddk::GetProtocolable, ddk::UnbindableDeprecated>;

class PartitionDevice : public PartitionDeviceType,
                        public ddk::BlockImplProtocol<PartitionDevice, ddk::base_protocol>,
                        public ddk::BlockPartitionProtocol<PartitionDevice>,
                        public fbl::RefCounted<PartitionDevice> {
 public:
  PartitionDevice(zx_device_t* parent, SdmmcBlockDevice* sdmmc_parent,
                  const block_info_t& block_info, EmmcPartition partition)
      : PartitionDeviceType(parent),
        sdmmc_parent_(sdmmc_parent),
        block_info_(block_info),
        partition_(partition) {}

  zx_status_t AddDevice();

  void DdkUnbindDeprecated();
  void DdkRelease();

  zx_off_t DdkGetSize();
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);

  void BlockImplQuery(block_info_t* info_out, size_t* block_op_size_out);
  void BlockImplQueue(block_op_t* btxn, block_impl_queue_callback completion_cb, void* cookie);

  zx_status_t BlockPartitionGetGuid(guidtype_t guid_type, guid_t* out_guid);
  zx_status_t BlockPartitionGetName(char* out_name, size_t capacity);

 private:
  SdmmcBlockDevice* const sdmmc_parent_;
  const block_info_t block_info_;
  const EmmcPartition partition_;
  std::atomic<bool> dead_ = false;
};

class SdmmcBlockDevice;
using SdmmcBlockDeviceType = ddk::Device<SdmmcBlockDevice, ddk::UnbindableDeprecated>;

class SdmmcBlockDevice : public SdmmcBlockDeviceType, public fbl::RefCounted<SdmmcBlockDevice> {
 public:
  SdmmcBlockDevice(zx_device_t* parent, const SdmmcDevice& sdmmc)
      : SdmmcBlockDeviceType(parent), sdmmc_(sdmmc) {
    block_info_.max_transfer_size = static_cast<uint32_t>(sdmmc_.host_info().max_transfer_size);
  }
  ~SdmmcBlockDevice() { txn_list_.CompleteAll(ZX_ERR_INTERNAL); }

  static zx_status_t Create(zx_device_t* parent, const SdmmcDevice& sdmmc,
                            fbl::RefPtr<SdmmcBlockDevice>* out_dev);

  zx_status_t ProbeSd();
  zx_status_t ProbeMmc();

  zx_status_t AddDevice();

  void DdkUnbindDeprecated();
  void DdkRelease();

  // Called by children of this device.
  void Queue(BlockOperation txn);

  // Visible for testing.
  zx_status_t Init() { return sdmmc_.Init(); }
  zx_status_t StartWorkerThread();
  void StopWorkerThread();

  ddk::BlockImplProtocolClient GetBlockClient(size_t index) {
    block_impl_protocol_t proto = {};
    if (partitions_[index] &&
        partitions_[index]->DdkGetProtocol(ZX_PROTOCOL_BLOCK_IMPL, &proto) == ZX_OK) {
      return ddk::BlockImplProtocolClient(&proto);
    }
    return ddk::BlockImplProtocolClient();
  }

 private:
  void BlockComplete(BlockOperation* txn, zx_status_t status, trace_async_id_t async_id);
  void DoTxn(BlockOperation* txn);
  int WorkerThread();

  zx_status_t WaitForTran();

  zx_status_t MmcDoSwitch(uint8_t index, uint8_t value);
  zx_status_t MmcSetBusWidth(sdmmc_bus_width_t bus_width, uint8_t mmc_ext_csd_bus_width);
  sdmmc_bus_width_t MmcSelectBusWidth();
  zx_status_t MmcSwitchTiming(sdmmc_timing_t new_timing);
  zx_status_t MmcSwitchFreq(uint32_t new_freq);
  zx_status_t MmcDecodeExtCsd(const uint8_t* raw_ext_csd);
  bool MmcSupportsHs();
  bool MmcSupportsHsDdr();
  bool MmcSupportsHs200();
  bool MmcSupportsHs400();

  std::atomic<trace_async_id_t> async_id_;

  SdmmcDevice sdmmc_;

  sdmmc_bus_width_t bus_width_;
  sdmmc_timing_t timing_;

  uint32_t clock_rate_;  // Bus clock rate

  // mmc
  uint32_t raw_cid_[4];
  uint32_t raw_csd_[4];
  uint8_t raw_ext_csd_[512];

  fbl::Mutex lock_;
  fbl::ConditionVariable worker_event_ TA_GUARDED(lock_);

  // blockio requests
  block::BorrowedOperationQueue<EmmcPartition> txn_list_ TA_GUARDED(lock_);

  // outstanding request (1 right now)
  sdmmc_req_t req_;

  thrd_t worker_thread_ = 0;

  std::atomic<bool> dead_ = false;

  block_info_t block_info_;

  bool is_sd_ = false;

  uint64_t boot_partition_block_count_ = 0;
  std::array<fbl::RefPtr<PartitionDevice>, PARTITION_COUNT> partitions_;
  EmmcPartition current_partition_ = EmmcPartition::USER_DATA_PARTITION;
};

}  // namespace sdmmc
