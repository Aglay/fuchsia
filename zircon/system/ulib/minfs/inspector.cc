// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/disk-inspector/common-types.h>
#include <sys/stat.h>

#include <block-client/cpp/block-device.h>
#include <fbl/unique_fd.h>
#include <minfs/bcache.h>
#include <minfs/inspector.h>

#include "inspector-private.h"

namespace minfs {

namespace {
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

}  // namespace

void InodeObject::GetValue(const void** out_buffer, size_t* out_buffer_size) const {
  ZX_DEBUG_ASSERT_MSG(false, "Invalid GetValue call for non primitive data type.");
}

std::unique_ptr<disk_inspector::DiskObject> InodeObject::GetElementAt(uint32_t index) const {
  switch (index) {
    case 0: {
      // uint32_t magic
      return CreateUint32DiskObj("magic", &(inode_.magic));
    }
    case 1: {
      // uint32_t size
      return CreateUint32DiskObj("size", &(inode_.size));
    }
    case 2: {
      // uint32_t block_count
      return CreateUint32DiskObj("block_count", &(inode_.block_count));
    }
    case 3: {
      // uint32_t link_count
      return CreateUint32DiskObj("link_count", &(inode_.link_count));
    }
    case 4: {
      // uint64_t create_time
      return CreateUint64DiskObj("create_time", &(inode_.create_time));
    }
    case 5: {
      // uint64_t modify_time
      return CreateUint64DiskObj("modify_time", &(inode_.modify_time));
    }
    case 6: {
      // uint32_t seq_num
      return CreateUint32DiskObj("seq_num", &(inode_.seq_num));
    }
    case 7: {
      // uint32_t gen_num
      return CreateUint32DiskObj("gen_num", &(inode_.gen_num));
    }
    case 8: {
      // uint32_t dirent_count
      return CreateUint32DiskObj("dirent_count", &(inode_.dirent_count));
    }
    case 9: {
      // ino_t/uint32_t last_inode
      return CreateUint32DiskObj("last_inode", &(inode_.last_inode));
    }
    case 10: {
      // ino_t/uint32_t next_inode
      return CreateUint32DiskObj("next_inode", &(inode_.next_inode));
    }
    case 11: {
      // uint32_t Array rsvd
      return CreateUint32ArrayDiskObj("reserved", inode_.rsvd, 3);
    }
    case 12: {
      // blk_t/uint32_t Array dnum
      return CreateUint32ArrayDiskObj("direct blocks", inode_.dnum, kMinfsDirect);
    }
    case 13: {
      // blk_t/uint32_t Array inum
      return CreateUint32ArrayDiskObj("indirect blocks", inode_.inum, kMinfsIndirect);
    }
    case 14: {
      // blk_t/uint32_t Array dinum
      return CreateUint32ArrayDiskObj("double indirect blocks", inode_.dinum, kMinfsDoublyIndirect);
    }
  }
  return nullptr;
}

void InodeTableObject::GetValue(const void** out_buffer, size_t* out_buffer_size) const {
  ZX_DEBUG_ASSERT_MSG(false, "Invalid GetValue call for non primitive data type.");
}

std::unique_ptr<disk_inspector::DiskObject> InodeTableObject::GetElementAt(uint32_t index) const {
  if (index >= inode_count_) {
    return nullptr;
  }
  return GetInode(static_cast<ino_t>(index));
}

std::unique_ptr<disk_inspector::DiskObject> InodeTableObject::GetInode(ino_t inode) const {
  Inode inode_obj;
  inode_table_->Load(inode, &inode_obj);
  return std::unique_ptr<disk_inspector::DiskObject>(new InodeObject(inode_obj));
}

void SuperBlockObject::GetValue(const void** out_buffer, size_t* out_buffer_size) const {
  ZX_DEBUG_ASSERT_MSG(false, "Invalid GetValue call for non primitive data type.");
}

std::unique_ptr<disk_inspector::DiskObject> SuperBlockObject::GetElementAt(uint32_t index) const {
  switch (index) {
    case 0: {
      // uint64_t magic0.
      return CreateUint64DiskObj("magic0", &(sb_.magic0));
    }
    case 1: {
      // uint64_t magic1.
      return CreateUint64DiskObj("magic1", &(sb_.magic1));
    }
    case 2: {
      // uint32_t version_major.
      return CreateUint32DiskObj("version_major", &(sb_.version_major));
    }
    case 3: {
      // uint32_t version_minor.
      return CreateUint32DiskObj("version_minor", &(sb_.version_minor));
    }
    case 4: {
      // uint32_t flags.
      return CreateUint32DiskObj("flags", &(sb_.flags));
    }
    case 5: {
      // uint32_t block_size.
      return CreateUint32DiskObj("block_size", &(sb_.block_size));
    }
    case 6: {
      // uint32_t inode_size.
      return CreateUint32DiskObj("inode_size", &(sb_.inode_size));
    }
    case 7: {
      // uint32_t block_count.
      return CreateUint32DiskObj("block_count", &(sb_.block_count));
    }
    case 8: {
      // uint32_t inode_count.
      return CreateUint32DiskObj("inode_count", &(sb_.inode_count));
    }
    case 9: {
      // uint32_t alloc_block_count.
      return CreateUint32DiskObj("alloc_block_count", &(sb_.alloc_block_count));
    }
    case 10: {
      // uint32_t alloc_inode_count.
      return CreateUint32DiskObj("alloc_inode_count", &(sb_.alloc_inode_count));
    }
    case 11: {
      // uint32_t/blk_t ibm_block.
      return CreateUint32DiskObj("ibm_block", &(sb_.ibm_block));
    }
    case 12: {
      // uint32_t/blk_t abm_block.
      return CreateUint32DiskObj("abm_block", &(sb_.abm_block));
    }
    case 13: {
      // uint32_t/blk_t ino_block.
      return CreateUint32DiskObj("ino_block", &(sb_.ino_block));
    }
    case 14: {
      // uint32_t/blk_t integrity_start_block.
      return CreateUint32DiskObj("integrity_start_block", &(sb_.integrity_start_block));
    }
    case 15: {
      // uint32_t/blk_t dat_block.
      return CreateUint32DiskObj("dat_block", &(sb_.dat_block));
    }
    case 16: {
      // uint32_t slice_size.
      return CreateUint32DiskObj("slice_size", &(sb_.slice_size));
    }
    case 17: {
      // uint32_t vslice_count.
      return CreateUint32DiskObj("vslice_count", &(sb_.vslice_count));
    }
    case 18: {
      // uint32_t ibm_slices.
      return CreateUint32DiskObj("ibm_slices", &(sb_.ibm_slices));
    }
    case 19: {
      // uint32_t abm_slices.
      return CreateUint32DiskObj("abm_slices", &(sb_.abm_slices));
    }
    case 20: {
      // uint32_t ino_slices.
      return CreateUint32DiskObj("ino_slices", &(sb_.ino_slices));
    }
    case 21: {
      // uint32_t integrity_slices.
      return CreateUint32DiskObj("integrity_slices", &(sb_.integrity_slices));
    }
    case 22: {
      // uint32_t dat_slices.
      return CreateUint32DiskObj("dat_slices", &(sb_.dat_slices));
    }
    case 23: {
      // uint32_t/ino_t unlinked_head.
      return CreateUint32DiskObj("unlinked_head", &(sb_.unlinked_head));
    }
    case 24: {
      // uint32_t/ino_t unlinked_tail.
      return CreateUint32DiskObj("unlinked_tail", &(sb_.unlinked_tail));
    }
    case 25: {
      //  uint32_t checksum.
      return CreateUint32DiskObj("checksum", &(sb_.checksum));
    }
    case 26: {
      //  uint32_t generation_count.
      return CreateUint32DiskObj("generation_count", &(sb_.generation_count));
    }
    case 27: {
      // uint32_t reserved[].
      return CreateUint32ArrayDiskObj("reserved", sb_.reserved, 1);
    }
  }
  return nullptr;
}

void JournalObject::GetValue(const void** out_buffer, size_t* out_buffer_size) const {
  ZX_DEBUG_ASSERT_MSG(false, "Invalid GetValue call for non primitive data type.");
}

std::unique_ptr<disk_inspector::DiskObject> JournalObject::GetElementAt(uint32_t index) const {
  switch (index) {
    case 0: {
      // uint64_t magic.
      return CreateUint64DiskObj("magic", &(journal_info_->magic));
    }
    case 1: {
      // uint64_t start_block
      return CreateUint64DiskObj("start_block", &(journal_info_->start_block));
    }
    case 2: {
      // uint64_t reserved
      return CreateUint64DiskObj("reserved", &(journal_info_->reserved));
    }
    case 3: {
      // uint64_t timestamp
      return CreateUint64DiskObj("timestamp", &(journal_info_->timestamp));
    }
    case 4: {
      // uint64_t checksum
      return CreateUint32DiskObj("checksum", &(journal_info_->checksum));
    }
  }
  return nullptr;
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
  zx_status_t status = ZX_OK;
  char data[kMinfsBlockSize];
  if (bc->Readblk(kSuperblockStart, data) < 0) {
    FS_TRACE_ERROR("minfsInspector: could not read superblock\n");
    return ZX_ERR_IO;
  }
  Superblock* info = reinterpret_cast<Superblock*>(data);
  std::unique_ptr<Minfs> fs;
  if ((status = Minfs::Create(std::move(bc), info, IntegrityCheck::kNone, &fs)) != ZX_OK) {
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

  if (fs_->ReadBlock(fs_->Info().integrity_start_block, data) < 0) {
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
