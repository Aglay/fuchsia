// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "partition-client.h"

#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <zircon/status.h>

#include "pave-logging.h"

namespace paver {
namespace {

namespace block = ::llcpp::fuchsia::hardware::block;
namespace skipblock = ::llcpp::fuchsia::hardware::skipblock;

}  // namespace

zx_status_t BlockPartitionClient::ReadBlockInfo() {
  if (!block_info_) {
    auto result = partition_.GetInfo();
    zx_status_t status = result.ok() ? result->status : result.status();
    if (status != ZX_OK) {
      ERROR("Failed to get partition info with status: %d\n", status);
      return status;
    }
    block_info_ = *result->info;
  }
  return ZX_OK;
}

zx_status_t BlockPartitionClient::GetBlockSize(size_t* out_size) {
  zx_status_t status = ReadBlockInfo();
  if (status != ZX_OK) {
    return status;
  }
  *out_size = block_info_->block_size;
  return ZX_OK;
}

zx_status_t BlockPartitionClient::GetPartitionSize(size_t* out_size) {
  zx_status_t status = ReadBlockInfo();
  if (status != ZX_OK) {
    return status;
  }
  *out_size = block_info_->block_size * block_info_->block_count;
  return ZX_OK;
}

zx_status_t BlockPartitionClient::RegisterFastBlockIo() {
  if (client_) {
    return ZX_OK;
  }

  auto result = partition_.GetFifo();
  zx_status_t status = result.ok() ? result->status : result.status();
  if (status != ZX_OK) {
    return status;
  }

  block_client::Client client;
  status = block_client::Client::Create(std::move(result->fifo), &client);
  if (status != ZX_OK) {
    return status;
  }

  client_ = std::move(client);
  return ZX_OK;
}

zx_status_t BlockPartitionClient::RegisterVmo(const zx::vmo& vmo, vmoid_t* out_vmoid) {
  zx::vmo dup;
  if (vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup) != ZX_OK) {
    ERROR("Couldn't duplicate buffer vmo\n");
    return ZX_ERR_IO;
  }

  auto result = partition_.AttachVmo(std::move(dup));
  zx_status_t status = result.ok() ? result->status : result.status();
  if (status != ZX_OK) {
    return status;
  }

  *out_vmoid = result->vmoid->id;
  return ZX_OK;
}

zx_status_t BlockPartitionClient::Setup(const zx::vmo& vmo, vmoid_t* out_vmoid) {
  zx_status_t status = RegisterFastBlockIo();
  if (status != ZX_OK) {
    return status;
  }

  status = RegisterVmo(vmo, out_vmoid);
  if (status != ZX_OK) {
    return status;
  }

  size_t block_size;
  status = GetBlockSize(&block_size);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

zx_status_t BlockPartitionClient::Read(const zx::vmo& vmo, size_t size) {
  vmoid_t vmoid;
  zx_status_t status = Setup(vmo, &vmoid);
  if (status != ZX_OK) {
    return status;
  }

  block_fifo_request_t request;
  request.group = 0;
  request.vmoid = vmoid;
  request.opcode = BLOCKIO_READ;

  const uint64_t length = size / block_info_->block_size;
  if (length > UINT32_MAX) {
    ERROR("Error reading partition data: Too large\n");
    return ZX_ERR_OUT_OF_RANGE;
  }
  request.length = static_cast<uint32_t>(length);
  request.vmo_offset = 0;
  request.dev_offset = 0;

  if ((status = client_->Transaction(&request, 1)) != ZX_OK) {
    ERROR("Error reading partition data: %s\n", zx_status_get_string(status));
    return status;
  }

  return ZX_OK;
}

zx_status_t BlockPartitionClient::Write(const zx::vmo& vmo, size_t vmo_size) {
  vmoid_t vmoid;
  zx_status_t status = Setup(vmo, &vmoid);
  if (status != ZX_OK) {
    return status;
  }

  block_fifo_request_t request;
  request.group = 0;
  request.vmoid = vmoid;
  request.opcode = BLOCKIO_WRITE;

  uint64_t length = vmo_size / block_info_->block_size;
  if (length > UINT32_MAX) {
    ERROR("Error writing partition data: Too large\n");
    return ZX_ERR_OUT_OF_RANGE;
  }
  request.length = static_cast<uint32_t>(length);
  request.vmo_offset = 0;
  request.dev_offset = 0;

  if ((status = client_->Transaction(&request, 1)) != ZX_OK) {
    ERROR("Error writing partition data: %s\n", zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

zx_status_t BlockPartitionClient::Flush() {
  zx_status_t status = RegisterFastBlockIo();
  if (status != ZX_OK) {
    return status;
  }

  block_fifo_request_t request;
  request.group = 0;
  request.vmoid = BLOCK_VMOID_INVALID;
  request.opcode = BLOCKIO_FLUSH;
  request.length = 0;
  request.vmo_offset = 0;
  request.dev_offset = 0;

  return client_->Transaction(&request, 1);
}

zx::channel BlockPartitionClient::GetChannel() {
  zx::channel channel(fdio_service_clone(partition_.channel().get()));
  return channel;
}

fbl::unique_fd BlockPartitionClient::block_fd() {
  zx::channel dup(fdio_service_clone(partition_.channel().get()));

  int block_fd;
  zx_status_t status = fdio_fd_create(dup.release(), &block_fd);
  if (status != ZX_OK) {
    return fbl::unique_fd();
  }
  return fbl::unique_fd(block_fd);
}

zx_status_t SkipBlockPartitionClient::ReadPartitionInfo() {
  if (!partition_info_) {
    auto result = partition_.GetPartitionInfo();
      zx_status_t status = result.ok() ? result->status : result.status();
      if (status != ZX_OK) {
      ERROR("Failed to get partition info with status: %d\n", status);
      return status;
    }
    partition_info_ = result->partition_info;
  }
  return ZX_OK;
}

zx_status_t SkipBlockPartitionClient::GetBlockSize(size_t* out_size) {
  zx_status_t status = ReadPartitionInfo();
  if (status != ZX_OK) {
    return status;
  }
  *out_size = static_cast<size_t>(partition_info_->block_size_bytes);
  return ZX_OK;
}

zx_status_t SkipBlockPartitionClient::GetPartitionSize(size_t* out_size) {
  zx_status_t status = ReadPartitionInfo();
  if (status != ZX_OK) {
    return status;
  }
  *out_size = partition_info_->block_size_bytes * partition_info_->partition_block_count;
  return ZX_OK;
}

zx_status_t SkipBlockPartitionClient::Read(const zx::vmo& vmo, size_t size) {
  size_t block_size;
  zx_status_t status = GetBlockSize(&block_size);
  if (status != ZX_OK) {
    return status;
  }

  zx::vmo dup;
  if ((status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup)) != ZX_OK) {
    ERROR("Couldn't duplicate buffer vmo\n");
    return status;
  }

  skipblock::ReadWriteOperation operation = {
      .vmo = std::move(dup),
      .vmo_offset = 0,
      .block = 0,
      .block_count = static_cast<uint32_t>(size / block_size),
  };

  auto result = partition_.Read(std::move(operation));
  status = result.ok() ? result->status : result.status();
  if (!result.ok()) {
    ERROR("Error reading partition data: %s\n", zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

zx_status_t SkipBlockPartitionClient::Write(const zx::vmo& vmo, size_t size) {
  size_t block_size;
  zx_status_t status = GetBlockSize(&block_size);
  if (status != ZX_OK) {
    return status;
  }

  zx::vmo dup;
  if ((status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup)) != ZX_OK) {
    ERROR("Couldn't duplicate buffer vmo\n");
    return status;
  }

  skipblock::ReadWriteOperation operation = {
      .vmo = std::move(dup),
      .vmo_offset = 0,
      .block = 0,
      .block_count = static_cast<uint32_t>(size / block_size),
  };

  auto result = partition_.Write(std::move(operation));
  status = result.ok() ? result->status : result.status();
  if (status != ZX_OK) {
    ERROR("Error writing partition data: %s\n", zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

zx_status_t SkipBlockPartitionClient::Flush() { return ZX_OK; }

zx::channel SkipBlockPartitionClient::GetChannel() { return {}; }

fbl::unique_fd SkipBlockPartitionClient::block_fd() { return fbl::unique_fd(); }

zx_status_t SysconfigPartitionClient::GetBlockSize(size_t* out_size) {
  *out_size =  client_.GetPartitionSize(partition_);
  return ZX_OK;
}

zx_status_t SysconfigPartitionClient::GetPartitionSize(size_t* out_size) {
  *out_size =  client_.GetPartitionSize(partition_);
  return ZX_OK;
}

zx_status_t SysconfigPartitionClient::Read(const zx::vmo& vmo, size_t size) {
  return client_.ReadPartition(partition_, vmo, 0);
}

zx_status_t SysconfigPartitionClient::Write(const zx::vmo& vmo, size_t size) {
  if (size != client_.GetPartitionSize(partition_)) {
    return ZX_ERR_INVALID_ARGS;
  }
  return client_.WritePartition(partition_, vmo, 0);
}

zx_status_t SysconfigPartitionClient::Flush() { return ZX_OK; }

zx::channel SysconfigPartitionClient::GetChannel() { return {}; }

fbl::unique_fd SysconfigPartitionClient::block_fd() { return fbl::unique_fd(); }

}  // namespace paver
