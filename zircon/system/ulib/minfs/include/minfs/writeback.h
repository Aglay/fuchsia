// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINFS_WRITEBACK_H_
#define MINFS_WRITEBACK_H_

#include <utility>
#include <vector>

#ifdef __Fuchsia__
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/zx/vmo.h>

#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fs/transaction/writeback.h>
#endif

#include <fbl/algorithm.h>
#include <fbl/intrusive_hash_table.h>
#include <fbl/intrusive_single_list.h>
#include <fbl/macros.h>
#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <fs/queue.h>
#include <fs/vfs.h>
#include <minfs/allocator-promise.h>
#include <minfs/bcache.h>
#include <minfs/block-txn.h>
#include <minfs/format.h>
#include <minfs/pending-work.h>

namespace minfs {

class DataAssignableVnode;
class InodeManager;
class TransactionalFs;
class VnodeMinfs;

// A wrapper around a WriteTxn, holding references to the underlying Vnodes
// corresponding to the txn, so their Vnodes (and VMOs) are not released
// while being written out to disk.
//
// Additionally, this class allows completions to be signalled when the transaction
// has successfully completed.
class WritebackWork : public WriteTxn,
                      public fbl::SinglyLinkedListable<fbl::unique_ptr<WritebackWork>> {
 public:
  WritebackWork(Bcache* bc);

  // Sets the WritebackWork to a completed state. |status| should indicate whether the work was
  // completed successfully.
  void MarkCompleted(zx_status_t status);

  // Allow "pinning" Vnodes so they aren't destroyed while we're completing
  // this writeback operation.
  void PinVnode(fbl::RefPtr<VnodeMinfs> vn);

  // Actually transacts the enqueued work, and resets the WritebackWork to
  // its initial state. Returns the result of the transaction.
  zx_status_t Complete();

 private:
  size_t node_count_;
  // May be empty. Currently '4' is the maximum number of vnodes within a
  // single unit of writeback work, which occurs during a cross-directory
  // rename operation.
  fbl::RefPtr<VnodeMinfs> vn_[4];
};

// Tracks the current transaction, including any enqueued writes, and reserved blocks
// and inodes. Also handles allocation of previously reserved blocks/inodes.
// Upon construction, acquires a lock to ensure that all work being done within the
// scope of the transaction is thread-safe. Specifically, the Minfs superblock, block bitmap, and
// inode table, as well as the Vnode block count and inode size may in the near future be modified
// asynchronously. Since these modifications require a Transaction to be in progress, this lock
// will protect against multiple simultaneous writes to these structures.
class Transaction final : public PendingWork {
 public:
  static zx_status_t Create(TransactionalFs* minfs, size_t reserve_inodes, size_t reserve_blocks,
                            InodeManager* inode_manager, Allocator* block_allocator,
                            fbl::unique_ptr<Transaction>* out);

  Transaction() = delete;

  explicit Transaction(TransactionalFs* minfs);

  ~Transaction() final;

  ////////////////
  // PendingWork interface.

  void EnqueueMetadata(WriteData source, fs::Operation operation) final;

  void EnqueueData(WriteData source, fs::Operation operation) final;

  ////////////////
  // Other methods.
  size_t AllocateInode() {
    ZX_DEBUG_ASSERT(inode_promise_.IsInitialized());
    return inode_promise_.Allocate(this);
  }

  size_t AllocateBlock() {
    ZX_DEBUG_ASSERT(block_promise_.IsInitialized());
    return block_promise_.Allocate(this);
  }

  void PinVnode(fbl::RefPtr<VnodeMinfs> vnode);

#ifdef __Fuchsia__
  // Returns a vector of all enqueued metadata write operations.
  fbl::Vector<fs::UnbufferedOperation> RemoveMetadataOperations() {
    return metadata_operations_.TakeOperations();
  }

  // Returns a vector of all enqueued data write operations.
  fbl::Vector<fs::UnbufferedOperation> RemoveDataOperations() {
    return data_operations_.TakeOperations();
  }

  size_t SwapBlock(size_t old_bno) {
    ZX_DEBUG_ASSERT(block_promise_.IsInitialized());
    return block_promise_.Swap(old_bno);
  }

  void Resolve() {
    if (block_promise_.IsInitialized()) {
      block_promise_.SwapCommit(this);
    }
  }

  // Removes |requested| blocks from block_promise_ and gives them to |other_promise|.
  void GiveBlocksToPromise(size_t requested, AllocatorPromise* other_promise) {
    ZX_DEBUG_ASSERT(block_promise_.IsInitialized());
    block_promise_.GiveBlocks(requested, other_promise);
  }

  // Removes |requested| blocks from |other_promise| and gives them to block_promise_.
  void MergeBlockPromise(AllocatorPromise* other_promise) {
    other_promise->GiveBlocks(other_promise->GetReserved(), &block_promise_);
  }

  std::vector<fbl::RefPtr<VnodeMinfs>> RemovePinnedVnodes();

#endif

 private:
#ifdef __Fuchsia__
  fbl::AutoLock<fbl::Mutex> lock_;
  fs::UnbufferedOperationsBuilder metadata_operations_;
  fs::UnbufferedOperationsBuilder data_operations_;
  std::vector<fbl::RefPtr<VnodeMinfs>> pinned_vnodes_;
#else
  WritebackWork* GetMetadataWork();
  WritebackWork* GetDataWork();

  fbl::unique_ptr<WritebackWork> metadata_work_;
  fbl::unique_ptr<WritebackWork> data_work_;
  Bcache* bc_;
#endif

  AllocatorPromise inode_promise_;
  AllocatorPromise block_promise_;
};

}  // namespace minfs

#endif  // MINFS_WRITEBACK_H_
