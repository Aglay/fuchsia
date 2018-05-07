// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_STORAGE_H_
#define PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_STORAGE_H_

#include <map>
#include <memory>
#include <set>
#include <vector>

#include <lib/async/dispatcher.h>

#include "lib/callback/capture.h"
#include "lib/fsl/socket/strings.h"
#include "lib/fxl/functional/closure.h"
#include "peridot/bin/ledger/cloud_sync/impl/testing/test_commit.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"
#include "peridot/bin/ledger/storage/testing/page_storage_empty_impl.h"

namespace cloud_sync {
// Fake implementation of storage::PageStorage. Injects the data that PageSync
// asks about: page id, existing unsynced commits to be retrieved through
// GetUnsyncedCommits() and new commits to be retrieved through GetCommit().
// Registers the commits marked as synced.
class TestPageStorage : public storage::PageStorageEmptyImpl {
 public:
  explicit TestPageStorage(async_t* async);

  std::unique_ptr<TestCommit> NewCommit(std::string id,
                                        std::string content,
                                        bool unsynced = true);

  storage::PageId GetId() override;

  void SetSyncDelegate(storage::PageSyncDelegate* page_sync_delegate) override;

  void GetHeadCommitIds(
      std::function<void(storage::Status, std::vector<storage::CommitId>)>
          callback) override;

  void GetCommit(storage::CommitIdView commit_id,
                 std::function<void(storage::Status,
                                    std::unique_ptr<const storage::Commit>)>
                     callback) override;

  void AddCommitsFromSync(
      std::vector<PageStorage::CommitIdAndBytes> ids_and_bytes,
      std::function<void(storage::Status status)> callback) override;

  void GetUnsyncedPieces(
      std::function<void(storage::Status,
                         std::vector<storage::ObjectIdentifier>)> callback)
      override;

  storage::Status AddCommitWatcher(storage::CommitWatcher* watcher) override;

  storage::Status RemoveCommitWatcher(storage::CommitWatcher* watcher) override;

  void GetUnsyncedCommits(
      std::function<void(storage::Status,
                         std::vector<std::unique_ptr<const storage::Commit>>)>
          callback) override;

  void MarkCommitSynced(const storage::CommitId& commit_id,
                        std::function<void(storage::Status)> callback) override;

  void SetSyncMetadata(fxl::StringView key,
                       fxl::StringView value,
                       std::function<void(storage::Status)> callback) override;

  void GetSyncMetadata(
      fxl::StringView key,
      std::function<void(storage::Status, std::string)> callback) override;

  storage::PageId page_id_to_return;
  // Commits to be returned from GetUnsyncedCommits calls.
  std::vector<std::unique_ptr<const storage::Commit>>
      unsynced_commits_to_return;
  size_t head_count = 1;
  // Commits to be returned from GetCommit() calls.
  std::map<storage::CommitId, std::unique_ptr<const storage::Commit>>
      new_commits_to_return;
  bool should_fail_get_unsynced_commits = false;
  bool should_fail_get_commit = false;
  bool should_fail_add_commit_from_sync = false;
  bool should_delay_add_commit_confirmation = false;
  std::vector<fxl::Closure> delayed_add_commit_confirmations;
  unsigned int add_commits_from_sync_calls = 0u;

  storage::PageSyncDelegate* page_sync_delegate_;
  std::set<storage::CommitId> commits_marked_as_synced;
  storage::CommitWatcher* watcher_;
  bool watcher_set = false;
  bool watcher_removed = false;
  std::map<storage::CommitId, std::string> received_commits;
  std::map<std::string, std::string> sync_metadata;

 private:
  async_t* const async_;
};

}  // namespace cloud_sync

#endif  // PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_STORAGE_H_
