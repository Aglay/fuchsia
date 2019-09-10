// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_IMPL_COMMIT_PRUNER_H_
#define SRC_LEDGER_BIN_STORAGE_IMPL_COMMIT_PRUNER_H_

#include "src/ledger/bin/environment/environment.h"
#include "src/ledger/bin/storage/impl/live_commit_tracker.h"
#include "src/ledger/bin/storage/public/page_storage.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/ledger/lib/coroutine/coroutine_manager.h"

namespace storage {

// Commit pruner computes which commits should be removed from the page storage.
class CommitPruner {
 public:
  class CommitPrunerDelegate {
   public:
    virtual ~CommitPrunerDelegate() = default;
    // Finds the commit with the given |commit_id| and calls the given |callback| with the result.
    // |PageStorage| must outlive any |Commit| obtained through it.
    virtual void GetCommit(CommitIdView commit_id,
                           fit::function<void(Status, std::unique_ptr<const Commit>)> callback) = 0;

    // Deletes the provided commits from local storage.
    virtual Status DeleteCommits(coroutine::CoroutineHandler* handler,
                                 std::vector<std::unique_ptr<const Commit>> commits) = 0;

    // Updates the clock entry for this device.
    virtual Status UpdateSelfClockEntry(coroutine::CoroutineHandler* handler,
                                        const ClockEntry& entry) = 0;
  };
  CommitPruner(CommitPrunerDelegate* delegate, LiveCommitTracker* commit_tracker,
               CommitPruningPolicy policy);
  ~CommitPruner();

  // Performs a pruning cycle.
  Status Prune(coroutine::CoroutineHandler* handler);

 private:
  // Finds the latest unique common ancestor among the live commits, as given by the
  // LiveCommitTracker.
  Status FindLatestUniqueCommonAncestorSync(coroutine::CoroutineHandler* handler,
                                            std::unique_ptr<const storage::Commit>* result);
  // Returns all locally-known ancestors of a commit.
  Status GetAllAncestors(coroutine::CoroutineHandler* handler,
                         std::unique_ptr<const storage::Commit> base,
                         std::vector<std::unique_ptr<const storage::Commit>>* result);

  CommitPrunerDelegate* const delegate_;
  LiveCommitTracker* const commit_tracker_;

  // Policy for pruning commits. By default, we don't prune.
  CommitPruningPolicy const policy_;
};

}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_IMPL_COMMIT_PRUNER_H_
