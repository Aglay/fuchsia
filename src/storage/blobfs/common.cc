// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/assert.h>

#include <limits>

#include <blobfs/blob-layout.h>
#include <blobfs/format.h>
#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <digest/node-digest.h>
#include <fs/trace.h>
#include <safemath/checked_math.h>

#ifdef __Fuchsia__
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>

#include <fvm/client.h>
#endif

#include <blobfs/common.h>

using digest::Digest;

namespace blobfs {

namespace {

// Dumps the content of superblock to |out|. Does nothing if |out| is nullptr.
void DumpSuperblock(const Superblock& info, FILE* out) {
  if (out == nullptr) {
    return;
  }

  fprintf(out,
          "info.magic0: %" PRIu64
          "\n"
          "info.magic1: %" PRIu64
          "\n"
          "info.format_version: %" PRIu32
          "\n"
          "info.flags: %" PRIu32
          "\n"
          "info.block_size: %" PRIu32
          "\n"
          "info.data_block_count: %" PRIu64
          "\n"
          "info.journal_block_count: %" PRIu64
          "\n"
          "info.inode_count: %" PRIu64
          "\n"
          "info.alloc_block_count: %" PRIu64
          "\n"
          "info.alloc_inode_count: %" PRIu64
          "\n"
          "info.slice_size: %" PRIu64
          "\n"
          "info.abm_slices: %" PRIu32
          "\n"
          "info.ino_slices: %" PRIu32
          "\n"
          "info.dat_slices: %" PRIu32
          "\n"
          "info.journal_slices: %" PRIu32
          "\n"
          "info.blob_layout_format: %" PRIu8
          "\n"
          "info.oldest_revision: %" PRIu64 "\n",
          info.magic0, info.magic1, info.format_version, info.flags, info.block_size,
          info.data_block_count, info.journal_block_count, info.inode_count, info.alloc_block_count,
          info.alloc_inode_count, info.slice_size, info.abm_slices, info.ino_slices,
          info.dat_slices, info.journal_slices, info.blob_layout_format, info.oldest_revision);
}

// Validates that this version of blobfs knows how to handle |format|.
bool IsValidBlobLayoutFormat(BlobLayoutFormat format) {
  switch (format) {
    case BlobLayoutFormat::kPaddedMerkleTreeAtStart:
    case BlobLayoutFormat::kCompactMerkleTreeAtEnd:
      return true;
  }
  return false;
}

}  // namespace

// Validate the metadata for the superblock, given a maximum number of
// available blocks.
zx_status_t CheckSuperblock(const Superblock* info, uint64_t max) {
  if ((info->magic0 != kBlobfsMagic0) || (info->magic1 != kBlobfsMagic1)) {
    FS_TRACE_ERROR("blobfs: bad magic\n");
    return ZX_ERR_INVALID_ARGS;
  }
  if (info->format_version != kBlobfsCurrentFormatVersion) {
    FS_TRACE_ERROR("blobfs: FS Version: %08x. Driver version: %08x\n", info->format_version,
                   kBlobfsCurrentFormatVersion);
    DumpSuperblock(*info, stderr);
    return ZX_ERR_INVALID_ARGS;
  }
  if (info->block_size != kBlobfsBlockSize) {
    FS_TRACE_ERROR("blobfs: bsz %u unsupported\n", info->block_size);
    DumpSuperblock(*info, stderr);
    return ZX_ERR_INVALID_ARGS;
  }

  if (info->data_block_count < kMinimumDataBlocks) {
    FS_TRACE_ERROR("blobfs: Not enough space for minimum data partition\n");
    return ZX_ERR_NO_SPACE;
  }

#ifdef __Fuchsia__
  if ((info->flags & kBlobFlagClean) == 0) {
    FS_TRACE_ERROR("blobfs: filesystem in dirty state. Was not unmounted cleanly.\n");
  } else {
    FS_TRACE_INFO("blobfs: filesystem in clean state.\n");
  }
#endif

  // Determine the number of blocks necessary for the block map and node map.
  uint64_t total_inode_size;
  if (mul_overflow(info->inode_count, sizeof(Inode), &total_inode_size)) {
    FS_TRACE_ERROR("Multiplication overflow");
    return ZX_ERR_OUT_OF_RANGE;
  }

  uint64_t node_map_size;
  if (mul_overflow(NodeMapBlocks(*info), kBlobfsBlockSize, &node_map_size)) {
    FS_TRACE_ERROR("Multiplication overflow");
    return ZX_ERR_OUT_OF_RANGE;
  }

  if (total_inode_size != node_map_size) {
    FS_TRACE_ERROR("blobfs: Inode table block must be entirely filled\n");
    return ZX_ERR_BAD_STATE;
  }

  if (info->journal_block_count < kMinimumJournalBlocks) {
    FS_TRACE_ERROR("blobfs: Not enough space for minimum journal partition\n");
    return ZX_ERR_NO_SPACE;
  }

  if ((info->flags & kBlobFlagFVM) == 0) {
    if (TotalBlocks(*info) > max) {
      FS_TRACE_ERROR("blobfs: too large for device\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    }
  } else {
    const size_t blocks_per_slice = info->slice_size / info->block_size;

    size_t abm_blocks_needed = BlockMapBlocks(*info);
    size_t abm_blocks_allocated = info->abm_slices * blocks_per_slice;
    if (abm_blocks_needed > abm_blocks_allocated) {
      FS_TRACE_ERROR("blobfs: Not enough slices for block bitmap\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    } else if (abm_blocks_allocated + BlockMapStartBlock(*info) >= NodeMapStartBlock(*info)) {
      FS_TRACE_ERROR("blobfs: Block bitmap collides into node map\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    }

    size_t ino_blocks_needed = NodeMapBlocks(*info);
    size_t ino_blocks_allocated = info->ino_slices * blocks_per_slice;
    if (ino_blocks_needed > ino_blocks_allocated) {
      FS_TRACE_ERROR("blobfs: Not enough slices for node map\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    } else if (ino_blocks_allocated + NodeMapStartBlock(*info) >= DataStartBlock(*info)) {
      FS_TRACE_ERROR("blobfs: Node bitmap collides into data blocks\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    }

    size_t dat_blocks_needed = DataBlocks(*info);
    size_t dat_blocks_allocated = info->dat_slices * blocks_per_slice;
    if (dat_blocks_needed < kStartBlockMinimum) {
      FS_TRACE_ERROR("blobfs: Partition too small; no space left for data blocks\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    } else if (dat_blocks_needed > dat_blocks_allocated) {
      FS_TRACE_ERROR("blobfs: Not enough slices for data blocks\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    } else if (dat_blocks_allocated + DataStartBlock(*info) >
               std::numeric_limits<uint32_t>::max()) {
      FS_TRACE_ERROR("blobfs: Data blocks overflow uint32\n");
      DumpSuperblock(*info, stderr);
      return ZX_ERR_INVALID_ARGS;
    }
  }

  if (!IsValidBlobLayoutFormat(static_cast<BlobLayoutFormat>(info->blob_layout_format))) {
    FS_TRACE_ERROR("blobfs: Unkown blob layout format: %u\n", info->blob_layout_format);
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_OK;
}

uint32_t CalculateVsliceCount(const Superblock& superblock) {
  // Account for an additional slice for the superblock itself.
  return safemath::checked_cast<uint32_t>(1 + static_cast<uint64_t>(superblock.abm_slices) +
                                          static_cast<uint64_t>(superblock.ino_slices) +
                                          static_cast<uint64_t>(superblock.dat_slices) +
                                          static_cast<uint64_t>(superblock.journal_slices));
}

uint32_t BlocksRequiredForInode(uint64_t inode_count) {
  return safemath::checked_cast<uint32_t>(fbl::round_up(inode_count, kBlobfsInodesPerBlock) /
                                          kBlobfsInodesPerBlock);
}

uint32_t BlocksRequiredForBits(uint64_t bit_count) {
  return safemath::checked_cast<uint32_t>(fbl::round_up(bit_count, kBlobfsBlockBits) /
                                          kBlobfsBlockBits);
}

uint32_t SuggestJournalBlocks(uint32_t current, uint32_t available) { return current + available; }

void InitializeSuperblock(uint64_t block_count, const FilesystemOptions& options,
                          Superblock* info) {
  uint64_t inodes = kBlobfsDefaultInodeCount;
  memset(info, 0x00, sizeof(*info));
  info->magic0 = kBlobfsMagic0;
  info->magic1 = kBlobfsMagic1;
  info->format_version = kBlobfsCurrentFormatVersion;
  info->flags = kBlobFlagClean;
  info->block_size = kBlobfsBlockSize;
  // TODO(planders): Consider modifying the inode count if we are low on space.
  //                 It doesn't make sense to have fewer data blocks than inodes.
  info->inode_count = inodes;
  info->alloc_block_count = kStartBlockMinimum;
  info->alloc_inode_count = 0;
  info->blob_layout_format =
      static_cast<decltype(Superblock::blob_layout_format)>(options.blob_layout_format);
  info->oldest_revision = options.oldest_revision;

  // Temporarily set the data_block_count to the total block_count so we can estimate the number
  // of pre-data blocks.
  info->data_block_count = block_count;

  // The result of DataStartBlock(info) is based on the current value of info.data_block_count.
  // As a result, the block bitmap may have slightly more space allocated than is necessary.
  size_t usable_blocks =
      JournalStartBlock(*info) < block_count ? block_count - JournalStartBlock(*info) : 0;

  // Determine allocation for the journal vs. data blocks based on the number of blocks remaining.
  if (usable_blocks >= kDefaultJournalBlocks * 2) {
    // Regular-sized partition, capable of fitting a data region
    // at least as large as the journal. Give all excess blocks
    // to the data region.
    info->journal_block_count = kDefaultJournalBlocks;
    info->data_block_count = usable_blocks - kDefaultJournalBlocks;
  } else if (usable_blocks >= kMinimumDataBlocks + kMinimumJournalBlocks) {
    // On smaller partitions, give both regions the minimum amount of space,
    // and split the remainder. The choice of where to allocate the "remainder"
    // is arbitrary.
    const size_t remainder_blocks = usable_blocks - (kMinimumDataBlocks + kMinimumJournalBlocks);
    const size_t remainder_for_journal = remainder_blocks / 2;
    const size_t remainder_for_data = remainder_blocks - remainder_for_journal;
    info->journal_block_count = kMinimumJournalBlocks + remainder_for_journal;
    info->data_block_count = kMinimumDataBlocks + remainder_for_data;
  } else {
    // Error, partition too small.
    info->journal_block_count = 0;
    info->data_block_count = 0;
  }
}

BlobLayoutFormat GetBlobLayoutFormat(const Superblock& info) {
  BlobLayoutFormat format = static_cast<BlobLayoutFormat>(info.blob_layout_format);
  ZX_ASSERT_MSG(IsValidBlobLayoutFormat(format),
                "Invalid blob layout format.  Use CheckSuperblock to validate the Superblock "
                "before using it.");
  return format;
}

constexpr char kBlobVmoNamePrefix[] = "blob";
constexpr char kBlobCompressedVmoNamePrefix[] = "blobCompressed";
constexpr char kBlobMerkleVmoNamePrefix[] = "blob-merkle";

void FormatBlobDataVmoName(uint32_t node_index, fbl::StringBuffer<ZX_MAX_NAME_LEN>* out) {
  out->Clear();
  out->AppendPrintf("%s-%x", kBlobVmoNamePrefix, node_index);
}

void FormatBlobCompressedVmoName(uint32_t node_index, fbl::StringBuffer<ZX_MAX_NAME_LEN>* out) {
  out->Clear();
  out->AppendPrintf("%s-%x", kBlobCompressedVmoNamePrefix, node_index);
}

void FormatBlobMerkleVmoName(uint32_t node_index, fbl::StringBuffer<ZX_MAX_NAME_LEN>* out) {
  out->Clear();
  out->AppendPrintf("%s-%x", kBlobMerkleVmoNamePrefix, node_index);
}

}  // namespace blobfs
