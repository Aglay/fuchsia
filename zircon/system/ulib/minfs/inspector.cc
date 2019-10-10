// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/disk-inspector/common-types.h>
#include <sys/stat.h>

#include <block-client/cpp/block-device.h>
#include <fbl/unique_fd.h>
#include <minfs/bcache.h>
#include <minfs/inspector.h>

#include "inspector-inode-table.h"
#include "inspector-journal.h"
#include "inspector-private.h"
#include "inspector-superblock.h"

namespace minfs {

std::unique_ptr<disk_inspector::DiskObjectUint64> CreateUint64DiskObj(fbl::String fieldName,
                                                                      const uint64_t* value) {
  return std::make_unique<disk_inspector::DiskObjectUint64>(fieldName, value);
}

std::unique_ptr<disk_inspector::DiskObjectUint32> CreateUint32DiskObj(fbl::String fieldName,
                                                                      const uint32_t* value) {
  return std::make_unique<disk_inspector::DiskObjectUint32>(fieldName, value);
}

std::unique_ptr<disk_inspector::DiskObjectUint64Array> CreateUint64ArrayDiskObj(
    fbl::String fieldName, const uint64_t* value, size_t size) {
  return std::make_unique<disk_inspector::DiskObjectUint64Array>(fieldName, value, size);
}

std::unique_ptr<disk_inspector::DiskObjectUint32Array> CreateUint32ArrayDiskObj(
    fbl::String fieldName, const uint32_t* value, size_t size) {
  return std::make_unique<disk_inspector::DiskObjectUint32Array>(fieldName, value, size);
}

zx_status_t Inspector::GetRoot(std::unique_ptr<disk_inspector::DiskObject>* out) {
  struct stat stats;
  if (fstat(fd_.get(), &stats) < 0) {
    fprintf(stderr, "minfsInspector: could not find end of file/device\n");
    return ZX_ERR_IO;
  }

  if (stats.st_size == 0) {
    fprintf(stderr, "minfsInspector: invalid disk size\n");
    return ZX_ERR_IO;
  }

  size_t size = stats.st_size / minfs::kMinfsBlockSize;

  std::unique_ptr<block_client::BlockDevice> device;
  zx_status_t status = minfs::FdToBlockDevice(fd_, &device);
  if (status != ZX_OK) {
    fprintf(stderr, "fshost: Cannot convert fd to block device: %d\n", status);
    return status;
  }

  std::unique_ptr<minfs::Bcache> bc;
  status = minfs::Bcache::Create(std::move(device), static_cast<uint32_t>(size), &bc);
  if (status != ZX_OK) {
    fprintf(stderr, "minfsInspector: cannot create block cache\n");
    return status;
  }

  status = CreateRoot(std::move(bc), out);
  if (status != ZX_OK) {
    fprintf(stderr, "minfsInspector: cannot create root object\n");
    return status;
  }
  return ZX_OK;
}

zx_status_t Inspector::CreateRoot(std::unique_ptr<Bcache> bc,
                                  std::unique_ptr<disk_inspector::DiskObject>* out) {
  MountOptions options = {};
  options.readonly_after_initialization = true;
  options.repair_filesystem = false;
  options.use_journal = false;
  std::unique_ptr<Minfs> fs;
  zx_status_t status = Minfs::Create(std::move(bc), options, &fs);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("minfsInspector: Create Failed to Create Minfs: %d\n", status);
    return status;
  }
  *out = std::unique_ptr<disk_inspector::DiskObject>(new RootObject(std::move(fs)));
  return ZX_OK;
}

std::unique_ptr<disk_inspector::DiskObject> RootObject::GetSuperBlock() const {
  return std::unique_ptr<disk_inspector::DiskObject>(new SuperBlockObject(fs_->Info()));
}

std::unique_ptr<disk_inspector::DiskObject> RootObject::GetInodeTable() const {
  return std::unique_ptr<disk_inspector::DiskObject>(
      new InodeTableObject(fs_->GetInodeManager(), fs_->Info().alloc_inode_count));
}

std::unique_ptr<disk_inspector::DiskObject> RootObject::GetJournalInfo() const {
  char data[kMinfsBlockSize];

  if (fs_->ReadBlock(static_cast<blk_t>(JournalStartBlock(fs_->Info())), data) < 0) {
    FS_TRACE_ERROR("minfsInspector: could not read journal block\n");
    return nullptr;
  }

  fs::JournalInfo* info = reinterpret_cast<fs::JournalInfo*>(data);
  std::unique_ptr<fs::JournalInfo> journal_info(new fs::JournalInfo);
  memcpy(journal_info.get(), info, sizeof(*info));
  return std::unique_ptr<disk_inspector::DiskObject>(new JournalObject(std::move(journal_info)));
}

void RootObject::GetValue(const void** out_buffer, size_t* out_buffer_size) const {
  ZX_DEBUG_ASSERT_MSG(false, "Invalid GetValue call for non primitive data type.");
}

std::unique_ptr<disk_inspector::DiskObject> RootObject::GetElementAt(uint32_t index) const {
  switch (index) {
    case 0: {
      // Super Block
      return GetSuperBlock();
    }
    case 1: {
      // Inode Table
      return GetInodeTable();
    }
    case 2: {
      // Journal
      return GetJournalInfo();
    }
  };
  return nullptr;
}

}  // namespace minfs
