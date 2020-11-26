// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/pager/user-pager.h"

#include <fuchsia/scheduler/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/zx/thread.h>
#include <limits.h>
#include <zircon/status.h>
#include <zircon/threads.h>

#include <algorithm>
#include <memory>

#include <blobfs/format.h>
#include <fbl/auto_call.h>
#include <fs/trace.h>

#include "src/storage/blobfs/metrics.h"
#include "src/storage/lib/watchdog/include/lib/watchdog/operations.h"

namespace blobfs {
namespace pager {

UserPager::UserPager(BlobfsMetrics* metrics) : metrics_(metrics) {}

zx::status<std::unique_ptr<UserPager>> UserPager::Create(
    std::unique_ptr<TransferBuffer> buffer, std::unique_ptr<TransferBuffer> compressed_buffer,
    BlobfsMetrics* metrics) {
  ZX_DEBUG_ASSERT(metrics != nullptr && buffer != nullptr && buffer->vmo().is_valid() &&
                  compressed_buffer != nullptr && compressed_buffer->vmo().is_valid());

  TRACE_DURATION("blobfs", "UserPager::Create");

  auto pager = std::unique_ptr<UserPager>(new UserPager(metrics));
  pager->uncompressed_transfer_buffer_ = std::move(buffer);
  pager->compressed_transfer_buffer_ = std::move(compressed_buffer);

  zx_status_t status = pager->compressed_mapper_.Map(pager->compressed_transfer_buffer_->vmo(), 0,
                                                     kTransferBufferSize, ZX_VM_PERM_READ);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to map the compressed TransferBuffer: %s\n",
                   zx_status_get_string(status));
    return zx::error(status);
  }

  status = zx::vmo::create(kDecompressionBufferSize, 0, &pager->decompression_buffer_);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to create decompression buffer: %s\n",
                   zx_status_get_string(status));
    return zx::error(status);
  }

  // Create the pager object.
  status = zx::pager::create(0, &pager->pager_);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Cannot initialize pager\n");
    return zx::error(status);
  }

  // Start the pager thread.
  thrd_t thread;
  status = pager->pager_loop_.StartThread("blobfs-pager-thread", &thread);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Could not start pager thread\n");
    return zx::error(status);
  }

  // Set a scheduling deadline profile for the blobfs-pager-thread. This is purely a performance
  // optimization, and failure to do so is not fatal. So in the case of an error encountered
  // in any of the steps within |SetDeadlineProfile|, we log a warning, and successfully return the
  // UserPager instance.
  SetDeadlineProfile(thread);

  // Initialize and start the watchdog.
  pager->watchdog_ = fs_watchdog::CreateWatchdog();
  zx::status<> watchdog_status = pager->watchdog_->Start();
  if (!watchdog_status.is_ok()) {
    FS_TRACE_ERROR("blobfs: Could not start pager watchdog\n");
    return zx::error(watchdog_status.status_value());
  }

  return zx::ok(std::move(pager));
}

void UserPager::SetDeadlineProfile(thrd_t thread) {
  zx::channel channel0, channel1;
  zx_status_t status = zx::channel::create(0u, &channel0, &channel1);
  if (status != ZX_OK) {
    FS_TRACE_WARN("blobfs: Could not create channel pair: %s\n", zx_status_get_string(status));
    return;
  }

  // Connect to the scheduler profile provider service.
  status = fdio_service_connect(
      (std::string("/svc_blobfs/") + fuchsia::scheduler::ProfileProvider::Name_).c_str(),
      channel0.release());
  if (status != ZX_OK) {
    FS_TRACE_WARN("blobfs: Could not connect to scheduler profile provider: %s\n",
                  zx_status_get_string(status));
    return;
  }

  fuchsia::scheduler::ProfileProvider_SyncProxy provider(std::move(channel1));

  zx_status_t fidl_status = ZX_OK;
  zx::profile profile;

  // Deadline profile parameters for the pager thread.
  // Details on the performance analysis to arrive at these numbers can be found in fxbug.dev/56291.
  //
  // TODO(fxbug.dev/40858): Migrate to the role-based API when available, instead of hard
  // coding parameters.
  const zx_duration_t capacity = ZX_USEC(1300);
  const zx_duration_t deadline = ZX_MSEC(2);
  const zx_duration_t period = deadline;

  status = provider.GetDeadlineProfile(
      capacity, deadline, period, "/boot/bin/blobfs:blobfs-pager-thread", &fidl_status, &profile);

  if (status != ZX_OK || fidl_status != ZX_OK) {
    FS_TRACE_WARN("blobfs: Failed to get deadline profile: %s, %s\n", zx_status_get_string(status),
                  zx_status_get_string(fidl_status));
  } else {
    auto pager_thread = zx::unowned_thread(thrd_get_zx_handle(thread));
    // Set the deadline profile.
    status = pager_thread->set_profile(profile, 0);
    if (status != ZX_OK) {
      FS_TRACE_WARN("blobfs: Failed to set deadline profile: %s\n", zx_status_get_string(status));
    }
  }
}

UserPager::ReadRange UserPager::GetBlockAlignedReadRange(const UserPagerInfo& info, uint64_t offset,
                                                         uint64_t length) {
  ZX_DEBUG_ASSERT(offset < info.data_length_bytes);
  // Clamp the range to the size of the blob.
  length = std::min(length, info.data_length_bytes - offset);

  // Align to the block size for verification. (In practice this means alignment to 8k).
  zx_status_t status = info.verifier->Align(&offset, &length);
  // This only happens if the info.verifier thinks that [offset,length) is out of range, which
  // will only happen if |verifier| was initialized with a different length than the rest of |info|
  // (which is a programming error).
  ZX_DEBUG_ASSERT(status == ZX_OK);

  ZX_DEBUG_ASSERT(offset % kBlobfsBlockSize == 0);
  ZX_DEBUG_ASSERT(length % kBlobfsBlockSize == 0 || offset + length == info.data_length_bytes);

  return {.offset = offset, .length = length};
}

UserPager::ReadRange UserPager::GetBlockAlignedExtendedRange(const UserPagerInfo& info,
                                                             uint64_t offset, uint64_t length) {
  // TODO(rashaeqbal): Consider making the cluster size dynamic once we have prefetch read
  // efficiency metrics from the kernel - i.e. what percentage of prefetched pages are actually
  // used. Note that dynamic prefetch sizing might not play well with compression, since we
  // always need to read in entire compressed frames.
  //
  // TODO(rashaeqbal): Consider extending the range backwards as well. Will need some way to track
  // populated ranges.
  //
  // Read in at least 32KB at a time. This gives us the best performance numbers w.r.t. memory
  // savings and observed latencies. Detailed results from experiments to tune this can be found in
  // fxbug.dev/48519.
  constexpr uint64_t kReadAheadClusterSize = (32 * (1 << 10));

  size_t read_ahead_offset = offset;
  size_t read_ahead_length = std::max(kReadAheadClusterSize, length);
  read_ahead_length = std::min(read_ahead_length, info.data_length_bytes - read_ahead_offset);

  // Align to the block size for verification. (In practice this means alignment to 8k).
  return GetBlockAlignedReadRange(info, read_ahead_offset, read_ahead_length);
}

PagerErrorStatus UserPager::TransferPagesToVmo(uint64_t offset, uint64_t length, const zx::vmo& vmo,
                                               const UserPagerInfo& info) {
  size_t end;
  if (add_overflow(offset, length, &end)) {
    FS_TRACE_ERROR("blobfs: pager transfer range would overflow (off=%lu, len=%lu)\n", offset,
                   length);
    return PagerErrorStatus::kErrBadState;
  }

  static const fs_watchdog::FsOperationType kOperation(
      fs_watchdog::FsOperationType::CommonFsOperation::PageFault, std::chrono::seconds(60));
  [[maybe_unused]] fs_watchdog::FsOperationTracker tracker(&kOperation, watchdog_.get());

  if (info.decompressor != nullptr) {
    return TransferChunkedPagesToVmo(offset, length, vmo, info);
  } else {
    return TransferUncompressedPagesToVmo(offset, length, vmo, info);
  }
}

PagerErrorStatus UserPager::TransferUncompressedPagesToVmo(uint64_t requested_offset,
                                                           uint64_t requested_length,
                                                           const zx::vmo& vmo,
                                                           const UserPagerInfo& info) {
  ZX_DEBUG_ASSERT(!info.decompressor);

  const auto [offset, length] =
      GetBlockAlignedExtendedRange(info, requested_offset, requested_length);

  TRACE_DURATION("blobfs", "UserPager::TransferUncompressedPagesToVmo", "offset", offset, "length",
                 length);

  auto decommit = fbl::MakeAutoCall([this, length = length]() {
    // Decommit pages in the transfer buffer that might have been populated. All blobs share the
    // same transfer buffer - this prevents data leaks between different blobs.
    uncompressed_transfer_buffer_->vmo().op_range(
        ZX_VMO_OP_DECOMMIT, 0, fbl::round_up(length, kBlobfsBlockSize), nullptr, 0);
  });

  // Read from storage into the transfer buffer.
  auto populate_status = uncompressed_transfer_buffer_->Populate(offset, length, info);
  if (!populate_status.is_ok()) {
    FS_TRACE_ERROR("blobfs: TransferUncompressed: Failed to populate transfer vmo: %s\n",
                   populate_status.status_string());
    return ToPagerErrorStatus(populate_status.status_value());
  }

  const uint64_t rounded_length = fbl::round_up<uint64_t, uint64_t>(length, PAGE_SIZE);

  // The block size is a multiple of the page size and |length| has already been block aligned.  If
  // |rounded_length| is greater than |length| then |length| isn't block aligned because it's at the
  // end of the blob.  In the compact layout the Merkle tree can share the last block of the data
  // and may have been read into the transfer buffer.  The Merkle tree needs to be removed before
  // transfering the pages to the destination VMO.
  static_assert(kBlobfsBlockSize % PAGE_SIZE == 0);
  if (rounded_length > length) {
    zx_status_t status = uncompressed_transfer_buffer_->vmo().op_range(
        ZX_VMO_OP_ZERO, length, rounded_length - length, nullptr, 0);
    if (status != ZX_OK) {
      FS_TRACE_ERROR(
          "blobfs: TransferUncompressed: Failed to remove Merkle tree from transfer buffer: %s\n",
          zx_status_get_string(status));
      return ToPagerErrorStatus(status);
    }
  }

  // Verify the pages read in.
  {
    fzl::VmoMapper mapping;
    // We need to unmap the transfer VMO before its pages can be transferred to the destination VMO,
    // via |zx_pager_supply_pages|.
    auto unmap = fbl::MakeAutoCall([&]() { mapping.Unmap(); });

    // Map the transfer VMO in order to pass the verifier a pointer to the data.
    zx_status_t status =
        mapping.Map(uncompressed_transfer_buffer_->vmo(), 0, rounded_length, ZX_VM_PERM_READ);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: TransferUncompressed: Failed to map transfer buffer: %s\n",
                     zx_status_get_string(status));
      return ToPagerErrorStatus(status);
    }

    status = info.verifier->VerifyPartial(mapping.start(), length, offset, rounded_length);
    if (status != ZX_OK) {
      FS_TRACE_ERROR("blobfs: TransferUncompressed: Failed to verify data: %s\n",
                     zx_status_get_string(status));
      return ToPagerErrorStatus(status);
    }
  }

  ZX_DEBUG_ASSERT(offset % PAGE_SIZE == 0);
  // Move the pages from the transfer buffer to the destination VMO.
  zx_status_t status =
      pager_.supply_pages(vmo, offset, rounded_length, uncompressed_transfer_buffer_->vmo(), 0);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: TransferUncompressed: Failed to supply pages to paged VMO: %s\n",
                   zx_status_get_string(status));
    return ToPagerErrorStatus(status);
  }

  fbl::String merkle_root_hash = info.verifier->digest().ToString();
  metrics_->IncrementPageIn(merkle_root_hash, offset, length);

  return PagerErrorStatus::kOK;
}

PagerErrorStatus UserPager::TransferChunkedPagesToVmo(uint64_t requested_offset,
                                                      uint64_t requested_length, const zx::vmo& vmo,
                                                      const UserPagerInfo& info) {
  ZX_DEBUG_ASSERT(info.decompressor);

  const auto [offset, length] = GetBlockAlignedReadRange(info, requested_offset, requested_length);

  zx::status<CompressionMapping> mapping_status =
      info.decompressor->MappingForDecompressedRange(offset, length);
  if (!mapping_status.is_ok()) {
    FS_TRACE_ERROR("blobfs: TransferChunked: Failed to find range for [%lu, %lu): %s\n", offset,
                   offset + length, mapping_status.status_string());
    return ToPagerErrorStatus(mapping_status.status_value());
  }
  CompressionMapping mapping = mapping_status.value();

  TRACE_DURATION("blobfs", "UserPager::TransferChunkedPagesToVmo", "offset",
                 mapping.decompressed_offset, "length", mapping.decompressed_length);

  // The compressed frame may not fall at a block aligned address, but we read in block aligned
  // chunks. This offset will be applied to the buffer we pass to decompression.
  // TODO(jfsulliv): Caching blocks which span frames may be useful for performance.
  size_t offset_of_compressed_data = mapping.compressed_offset % kBlobfsBlockSize;

  // Read from storage into the transfer buffer.
  size_t read_offset = fbl::round_down(mapping.compressed_offset, kBlobfsBlockSize);
  size_t read_len = (mapping.compressed_length + offset_of_compressed_data);

  auto decommit_compressed = fbl::MakeAutoCall([this, length = read_len]() {
    // Decommit pages in the transfer buffer that might have been populated. All blobs share the
    // same transfer buffer - this prevents data leaks between different blobs.
    compressed_transfer_buffer_->vmo().op_range(
        ZX_VMO_OP_DECOMMIT, 0, fbl::round_up(length, kBlobfsBlockSize), nullptr, 0);
  });

  auto populate_status = compressed_transfer_buffer_->Populate(read_offset, read_len, info);
  if (!populate_status.is_ok()) {
    FS_TRACE_ERROR("blobfs: TransferChunked: Failed to populate transfer vmo: %s\n",
                   populate_status.status_string());
    return ToPagerErrorStatus(populate_status.status_value());
  }

  auto decommit_decompressed = fbl::MakeAutoCall([this, length = mapping.decompressed_length]() {
    // Decommit pages in the decompression buffer that might have been populated. All blobs share
    // the same transfer buffer - this prevents data leaks between different blobs.
    decompression_buffer_.op_range(ZX_VMO_OP_DECOMMIT, 0, fbl::round_up(length, kBlobfsBlockSize),
                                   nullptr, 0);
  });

  // Map the decompression VMO.
  fzl::VmoMapper decompressed_mapper;
  if (zx_status_t status =
          decompressed_mapper.Map(decompression_buffer_, 0, mapping.decompressed_length,
                                  ZX_VM_PERM_READ | ZX_VM_PERM_WRITE) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: TransferChunked: Failed to map decompress buffer: %s\n",
                   zx_status_get_string(status));
    return ToPagerErrorStatus(status);
  }
  auto unmap_decompression = fbl::MakeAutoCall([&]() { decompressed_mapper.Unmap(); });

  // Decompress the data
  fs::Ticker ticker(metrics_->Collecting());
  size_t decompressed_size = mapping.decompressed_length;
  uint8_t* src = static_cast<uint8_t*>(compressed_mapper_.start()) + offset_of_compressed_data;
  zx_status_t status =
      info.decompressor->DecompressRange(decompressed_mapper.start(), &decompressed_size, src,
                                         mapping.compressed_length, mapping.decompressed_offset);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: TransferChunked: Failed to decompress: %s\n",
                   zx_status_get_string(status));
    return ToPagerErrorStatus(status);
  }
  metrics_->paged_read_metrics().IncrementDecompression(CompressionAlgorithm::CHUNKED,
                                                        decompressed_size, ticker.End());

  // Verify the decompressed pages.
  const uint64_t rounded_length =
      fbl::round_up<uint64_t, uint64_t>(mapping.decompressed_length, PAGE_SIZE);
  status = info.verifier->VerifyPartial(decompressed_mapper.start(), mapping.decompressed_length,
                                        mapping.decompressed_offset, rounded_length);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: TransferChunked: Failed to verify data: %s\n",
                   zx_status_get_string(status));
    return ToPagerErrorStatus(status);
  }

  decompressed_mapper.Unmap();

  // Move the pages from the decompression buffer to the destination VMO.
  status = pager_.supply_pages(vmo, mapping.decompressed_offset, rounded_length,
                               decompression_buffer_, 0);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: TransferChunked: Failed to supply pages to paged VMO: %s\n",
                   zx_status_get_string(status));
    return ToPagerErrorStatus(status);
  }

  fbl::String merkle_root_hash = info.verifier->digest().ToString();
  metrics_->IncrementPageIn(merkle_root_hash, read_offset, read_len);

  return PagerErrorStatus::kOK;
}

}  // namespace pager
}  // namespace blobfs
