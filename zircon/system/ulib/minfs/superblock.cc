// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <minfs/superblock.h>

#include <lib/cksum.h>
#include <stdlib.h>
#include <string.h>

#include <bitmap/raw-bitmap.h>

#ifdef __Fuchsia__
#include <lib/fzl/owned-vmo-mapper.h>
#endif

#include <utility>

#include <minfs/superblock.h>

namespace minfs {

#ifdef __Fuchsia__

SuperblockManager::SuperblockManager(const Superblock* info, fzl::OwnedVmoMapper mapper)
    : mapping_(std::move(mapper)) {}

#else

SuperblockManager::SuperblockManager(const Superblock* info) {
  memcpy(&info_blk_[0], info, sizeof(Superblock));
}

#endif

SuperblockManager::~SuperblockManager() = default;

#ifdef __Fuchsia__
// Static.
zx_status_t SuperblockManager::Create(block_client::BlockDevice* device, const Superblock* info,
                                      uint32_t max_blocks, IntegrityCheck checks,
                                      fbl::unique_ptr<SuperblockManager>* out) {
#else
// Static.
zx_status_t SuperblockManager::Create(const Superblock* info, uint32_t max_blocks,
                                      IntegrityCheck checks,
                                      fbl::unique_ptr<SuperblockManager>* out) {
#endif
  zx_status_t status = ZX_OK;
  if (checks == IntegrityCheck::kAll) {
#ifdef __Fuchsia__
    status = CheckSuperblock(info, device, max_blocks);
#else
    status = CheckSuperblock(info, max_blocks);
#endif
    if (status != ZX_OK) {
      FS_TRACE_ERROR("SuperblockManager::Create failed to check info: %d\n", status);
      return status;
    }
  }

#ifdef __Fuchsia__
  fzl::OwnedVmoMapper mapper;
  // Create the info vmo
  if ((status = mapper.CreateAndMap(kMinfsBlockSize, "minfs-superblock")) != ZX_OK) {
    return status;
  }

  fuchsia_hardware_block_VmoID info_vmoid;
  if ((status = device->BlockAttachVmo(mapper.vmo(), &info_vmoid)) != ZX_OK) {
    return status;
  }
  memcpy(mapper.start(), info, sizeof(Superblock));

  auto sb = fbl::unique_ptr<SuperblockManager>(new SuperblockManager(info, std::move(mapper)));
#else
  auto sb = fbl::unique_ptr<SuperblockManager>(new SuperblockManager(info));
#endif
  *out = std::move(sb);
  return ZX_OK;
}

void SuperblockManager::Write(PendingWork* transaction, UpdateBackupSuperblock write_backup) {
  UpdateChecksum(MutableInfo());
#ifdef __Fuchsia__
  auto data = mapping_.vmo().get();
#else
  auto data = &info_blk_[0];
#endif

  fs::Operation op = {
    .type = fs::OperationType::kWrite,
    .vmo_offset = 0,
    .dev_offset = kSuperblockStart,
    .length = 1,
  };
  transaction->EnqueueMetadata(data, std::move(op));

  if (write_backup == UpdateBackupSuperblock::kUpdate) {
    blk_t superblock_dev_offset = kNonFvmSuperblockBackup;

    if (MutableInfo()->flags & kMinfsFlagFVM) {
      superblock_dev_offset = kFvmSuperblockBackup;
    }

    fs::Operation op = {
      .type = fs::OperationType::kWrite,
      .vmo_offset = 0,
      .dev_offset = superblock_dev_offset,
      .length = 1,
    };
    transaction->EnqueueMetadata(data, std::move(op));
  }
}

}  // namespace minfs
