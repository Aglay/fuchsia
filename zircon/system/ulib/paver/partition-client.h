// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <fuchsia/hardware/block/llcpp/fidl.h>
#include <fuchsia/hardware/skipblock/llcpp/fidl.h>
#include <lib/sysconfig/sync-client.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <zircon/types.h>

#include <optional>

#include <block-client/cpp/client.h>
#include <fbl/unique_fd.h>

namespace paver {

// Interface to synchronously read/write to a partition.
class PartitionClient {
 public:
  // Returns the block size which the vmo provided to read/write should be aligned to.
  virtual zx_status_t GetBlockSize(size_t* out_size) = 0;

  // Returns the partition size.
  virtual zx_status_t GetPartitionSize(size_t* out_size) = 0;

  // Reads the specified size from the partition into |vmo|. |size| must be aligned to the block
  // size returned in `GetBlockSize`.
  virtual zx_status_t Read(const zx::vmo& vmo, size_t size) = 0;

  // Writes |vmo| into the partition. |vmo_size| must be aligned to the block size returned in
  // `GetBlockSize`.
  virtual zx_status_t Write(const zx::vmo& vmo, size_t vmo_size) = 0;

  // Flushes all previous operations to persistent storage.
  virtual zx_status_t Flush() = 0;

  // Returns a channel to the partition, when backed by a block device.
  virtual zx::channel GetChannel() = 0;

  // Returns a file descriptor representing the partition.
  // Will return an invalid fd if underlying partition is not a block device.
  virtual fbl::unique_fd block_fd() = 0;

  virtual ~PartitionClient() = default;
};

class BlockPartitionClient final : public PartitionClient {
 public:
  explicit BlockPartitionClient(zx::channel partition) : partition_(std::move(partition)) {}

  zx_status_t GetBlockSize(size_t* out_size) final;
  zx_status_t GetPartitionSize(size_t* out_size) final;
  zx_status_t Read(const zx::vmo& vmo, size_t size) final;
  zx_status_t Write(const zx::vmo& vmo, size_t vmo_size) final;
  zx_status_t Flush() final;
  zx::channel GetChannel() final;
  fbl::unique_fd block_fd() final;

  // No copy, no move.
  BlockPartitionClient(const BlockPartitionClient&) = delete;
  BlockPartitionClient& operator=(const BlockPartitionClient&) = delete;
  BlockPartitionClient(BlockPartitionClient&&) = delete;
  BlockPartitionClient& operator=(BlockPartitionClient&&) = delete;

 private:
  zx_status_t Setup(const zx::vmo& vmo, vmoid_t* out_vmoid);
  zx_status_t RegisterFastBlockIo();
  zx_status_t RegisterVmo(const zx::vmo& vmo, vmoid_t* out_vmoid);
  zx_status_t ReadBlockInfo();

  ::llcpp::fuchsia::hardware::block::Block::SyncClient partition_;
  std::optional<block_client::Client> client_;
  std::optional<::llcpp::fuchsia::hardware::block::BlockInfo> block_info_;
};

class SkipBlockPartitionClient final : public PartitionClient {
 public:
  explicit SkipBlockPartitionClient(zx::channel partition) : partition_(std::move(partition)) {}

  zx_status_t GetBlockSize(size_t* out_size) final;
  zx_status_t GetPartitionSize(size_t* out_size) final;
  zx_status_t Read(const zx::vmo& vmo, size_t size) final;
  zx_status_t Write(const zx::vmo& vmo, size_t vmo_size) final;
  zx_status_t Flush() final;
  zx::channel GetChannel() final;
  fbl::unique_fd block_fd() final;

  // No copy, no move.
  SkipBlockPartitionClient(const SkipBlockPartitionClient&) = delete;
  SkipBlockPartitionClient& operator=(const SkipBlockPartitionClient&) = delete;
  SkipBlockPartitionClient(SkipBlockPartitionClient&&) = delete;
  SkipBlockPartitionClient& operator=(SkipBlockPartitionClient&&) = delete;

 private:
  zx_status_t ReadPartitionInfo();

  ::llcpp::fuchsia::hardware::skipblock::SkipBlock::SyncClient partition_;
  std::optional<::llcpp::fuchsia::hardware::skipblock::PartitionInfo> partition_info_;
};

// Specialized client for talking to sub-partitions of the sysconfig partition.
class SysconfigPartitionClient final : public PartitionClient {
 public:
  SysconfigPartitionClient(::sysconfig::SyncClient client,
                           ::sysconfig::SyncClient::PartitionType partition)
      : client_(std::move(client)), partition_(partition) {}

  zx_status_t GetBlockSize(size_t* out_size) final;
  zx_status_t GetPartitionSize(size_t* out_size) final;
  zx_status_t Read(const zx::vmo& vmo, size_t size) final;
  zx_status_t Write(const zx::vmo& vmo, size_t vmo_size) final;
  zx_status_t Flush() final;
  zx::channel GetChannel() final;
  fbl::unique_fd block_fd() final;

  // No copy, no move.
  SysconfigPartitionClient(const SysconfigPartitionClient&) = delete;
  SysconfigPartitionClient& operator=(const SysconfigPartitionClient&) = delete;
  SysconfigPartitionClient(SysconfigPartitionClient&&) = delete;
  SysconfigPartitionClient& operator=(SysconfigPartitionClient&&) = delete;

 private:
  ::sysconfig::SyncClient client_;
  ::sysconfig::SyncClient::PartitionType partition_;
};

}  // namespace paver
