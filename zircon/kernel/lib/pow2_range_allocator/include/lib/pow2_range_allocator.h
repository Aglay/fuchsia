// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2016, Google, Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_POW2_RANGE_ALLOCATOR_INCLUDE_LIB_POW2_RANGE_ALLOCATOR_H_
#define ZIRCON_KERNEL_LIB_POW2_RANGE_ALLOCATOR_INCLUDE_LIB_POW2_RANGE_ALLOCATOR_H_

#include <err.h>
#include <sys/types.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

#include <kernel/mutex.h>

// Pow2RangeAllocator is a small utility class which partitions a set of
// ranges of integers into sub-ranges which are power of 2 in length and power
// of 2 aligned and then manages allocating and freeing the subranges for
// clients.  It is responsible for breaking larger sub-regions into smaller ones
// as needed for allocation, and for merging sub-regions into larger sub-regions
// as needed during free operations.
//
// Its primary use is as a utility library for plaforms who need to manage
// allocating blocks MSI IRQ IDs on behalf of the PCI bus driver, but could (in
// theory) be used for other things).

class Pow2RangeAllocator {
 public:
  constexpr Pow2RangeAllocator() = default;

  // Pow2RangeAllocators cannot be copied or moved.
  Pow2RangeAllocator(const Pow2RangeAllocator&) = delete;
  Pow2RangeAllocator(Pow2RangeAllocator&&) = delete;
  Pow2RangeAllocator& operator=(const Pow2RangeAllocator&) = delete;
  Pow2RangeAllocator& operator=(Pow2RangeAllocator&&) = delete;

  // Initialize the state of a pow2 range allocator.
  //
  // @param max_alloc_size The maximum size of a single contiguous allocation.
  // Must be a power of 2.
  //
  // @return A status code indicating the success or failure of the operation.
  zx_status_t Init(uint32_t max_alloc_size);

  // Free all of the state associated with a previously initialized pow2 range
  // allocator.
  void Free();

  // Add a range of uint32_ts to the pool of ranges to be allocated.
  //
  // @param state A pointer to the state structure to add the range to.
  // @param range_start The start of the uint32_t range.
  // @param range_len The length of the uint32_t range.
  //
  // @return A status code incidcating the success or failure of the operation.
  // Possible return values include
  // ++ ZX_ERR_INVALID_ARGS range_len is zero, or would cause the range to wrap the
  //    maximum range of a uint32_t.
  // ++ ZX_ERR_ALREADY_EXISTS the specified range overlaps with a range already added
  //    to the allocator.
  // ++ ZX_ERR_NO_MEMORY Not enough memory to allocate the bookkeeping required for
  //    managing the range.
  zx_status_t AddRange(uint32_t range_start, uint32_t range_len);

  // Attempt to allocate a range of uint32_ts from the available sub-ranges.  The
  // sizeo the allocated range must be a power of 2, and if the allocation
  // succeeds, it is guaranteed to be aligned on a power of 2 boundary matching it
  // size.
  //
  // @param size The requested size of the region.
  // @param out_range_start An out parameter which will hold the start of the
  // allocated range upon success.
  //
  // @return A status code indicating the success or failure of the operation.
  // Possible return values include
  // ++ ZX_ERR_INVALID_ARGS Multiple reasons, including...
  //    ++ size is zero.
  //    ++ size is not a power of two.
  //    ++ out_range_start is NULL.
  // ++ ZX_ERR_NO_RESOURCES No contiguous, aligned region could be found to satisfy
  //    the allocation request.
  // ++ ZX_ERR_NO_MEMORY A region could be found, but memory required for bookkeeping
  //    could not be allocated.
  zx_status_t AllocateRange(uint32_t size, uint32_t* out_range_start);

  // Free a range previously allocated using AllocateRange.
  //
  // @param range_start The start of the previously allocated range.
  // @param size The size of the previously allocated range.
  void FreeRange(uint32_t range_start, uint32_t size);

 private:
  struct Block {
    struct list_node node;
    uint32_t bucket;
    uint32_t start;
  };

  struct Range {
    struct list_node node;
    uint32_t start, len;
  };

  Block* GetUnusedBlock();
  static void FreeBlockList(struct list_node* block_list);
  static void FreeRangeList(struct list_node* range_list);
  void ReturnFreeBlock(Block* block, bool merge_allowed);

  DECLARE_MUTEX(Pow2RangeAllocator) lock_;
  struct list_node ranges_ = LIST_INITIAL_VALUE(ranges_);
  struct list_node unused_blocks_ = LIST_INITIAL_VALUE(unused_blocks_);
  struct list_node allocated_blocks_ = LIST_INITIAL_VALUE(allocated_blocks_);
  struct list_node* free_block_buckets_ = nullptr;
  uint32_t bucket_count_ = 0;
};

#endif  // ZIRCON_KERNEL_LIB_POW2_RANGE_ALLOCATOR_INCLUDE_LIB_POW2_RANGE_ALLOCATOR_H_
