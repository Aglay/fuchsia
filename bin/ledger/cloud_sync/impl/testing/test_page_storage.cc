// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/cloud_sync/impl/testing/test_page_storage.h"

#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <lib/async/cpp/task.h>

#include "garnet/lib/callback/capture.h"
#include "lib/fsl/socket/strings.h"
#include "lib/fxl/functional/closure.h"
#include "lib/fxl/functional/make_copyable.h"
#include "peridot/bin/ledger/cloud_sync/impl/testing/test_commit.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"
#include "peridot/bin/ledger/storage/testing/page_storage_empty_impl.h"

namespace cloud_sync {
TestPageStorage::TestPageStorage(async_t* async) : async_(async) {}

std::unique_ptr<TestCommit> TestPageStorage::NewCommit(std::string id,
                                                       std::string content,
                                                       bool unsynced) {
  auto commit = std::make_unique<TestCommit>(std::move(id), std::move(content));
  if (unsynced) {
    unsynced_commits_to_return.push_back(commit->Clone());
  }
  return commit;
}

storage::PageId TestPageStorage::GetId() {
  return page_id_to_return;
}

void TestPageStorage::SetSyncDelegate(
    storage::PageSyncDelegate* page_sync_delegate) {
  page_sync_delegate_ = page_sync_delegate;
}

void TestPageStorage::GetHeadCommitIds(
    std::function<void(storage::Status, std::vector<storage::CommitId>)>
        callback) {
  async::PostTask(async_, [this, callback = std::move(callback)] {
    // Current tests only rely on the number of heads, not on the actual
    // ids.
    callback(storage::Status::OK, std::vector<storage::CommitId>(head_count));
  });
}

void TestPageStorage::GetCommit(
    storage::CommitIdView commit_id,
    std::function<void(storage::Status, std::unique_ptr<const storage::Commit>)>
        callback) {
  if (should_fail_get_commit) {
    async::PostTask(async_, [callback = std::move(callback)] {
      callback(storage::Status::IO_ERROR, nullptr);
    });
    return;
  }

  async::PostTask(
      async_,
      [this, commit_id = commit_id.ToString(), callback = std::move(callback)] {
          callback(storage::Status::OK,
                   std::move(new_commits_to_return[commit_id]));
  });
  new_commits_to_return.erase(commit_id.ToString());
}

void TestPageStorage::AddCommitsFromSync(
    std::vector<PageStorage::CommitIdAndBytes> ids_and_bytes,
    std::function<void(storage::Status status)> callback) {
  add_commits_from_sync_calls++;

  if (should_fail_add_commit_from_sync) {
    async::PostTask(
      async_,
      [callback]() { callback(storage::Status::IO_ERROR); });
    return;
  }

  fxl::Closure confirm = fxl::MakeCopyable(
      [this, ids_and_bytes = std::move(ids_and_bytes), callback]() mutable {
        for (auto& commit : ids_and_bytes) {
          received_commits[commit.id] = std::move(commit.bytes);
          unsynced_commits_to_return.erase(
              std::remove_if(
                  unsynced_commits_to_return.begin(),
                  unsynced_commits_to_return.end(),
                  [commit_id = std::move(commit.id)](
                      const std::unique_ptr<const storage::Commit>& commit) {
                    return commit->GetId() == commit_id;
                  }),
              unsynced_commits_to_return.end());
        }
        async::PostTask(
          async_,
          [callback = std::move(callback)] { callback(storage::Status::OK); });
      });
  if (should_delay_add_commit_confirmation) {
    delayed_add_commit_confirmations.push_back(move(confirm));
    return;
  }
  async::PostTask(async_, confirm);
}

void TestPageStorage::GetUnsyncedPieces(
    std::function<void(storage::Status, std::vector<storage::ObjectIdentifier>)>
        callback) {
  async::PostTask(async_, [callback = std::move(callback)] {
    callback(storage::Status::OK, std::vector<storage::ObjectIdentifier>());
  });
}

storage::Status TestPageStorage::AddCommitWatcher(
    storage::CommitWatcher* watcher) {
  watcher_ = watcher;
  watcher_set = true;
  return storage::Status::OK;
}

storage::Status TestPageStorage::RemoveCommitWatcher(
    storage::CommitWatcher* /*watcher*/) {
  watcher_removed = true;
  return storage::Status::OK;
}

void TestPageStorage::GetUnsyncedCommits(
    std::function<void(storage::Status,
                       std::vector<std::unique_ptr<const storage::Commit>>)>
        callback) {
  if (should_fail_get_unsynced_commits) {
    async::PostTask(async_, [callback = std::move(callback)] {
      callback(storage::Status::IO_ERROR, {});
    });
    return;
  }
  std::vector<std::unique_ptr<const storage::Commit>> results;
  results.resize(unsynced_commits_to_return.size());
  std::transform(unsynced_commits_to_return.begin(),
                 unsynced_commits_to_return.end(), results.begin(),
                 [](const std::unique_ptr<const storage::Commit>& commit) {
                   return commit->Clone();
                 });
  async::PostTask(async_,
                  fxl::MakeCopyable([results = std::move(results),
                                     callback = std::move(callback)]() mutable {
                    callback(storage::Status::OK, std::move(results));
                  }));
}

void TestPageStorage::MarkCommitSynced(
    const storage::CommitId& commit_id,
    std::function<void(storage::Status)> callback) {
  unsynced_commits_to_return.erase(
      std::remove_if(
          unsynced_commits_to_return.begin(), unsynced_commits_to_return.end(),
          [&commit_id](const std::unique_ptr<const storage::Commit>& commit) {
            return commit->GetId() == commit_id;
          }),
      unsynced_commits_to_return.end());
  commits_marked_as_synced.insert(commit_id);
  async::PostTask(async_, [callback = std::move(callback)] {
    callback(storage::Status::OK);
  });
}

void TestPageStorage::SetSyncMetadata(
    fxl::StringView key,
    fxl::StringView value,
    std::function<void(storage::Status)> callback) {
  sync_metadata[key.ToString()] = value.ToString();
  async::PostTask(async_, [callback = std::move(callback)] {
    callback(storage::Status::OK);
  });
}

void TestPageStorage::GetSyncMetadata(
    fxl::StringView key,
    std::function<void(storage::Status, std::string)> callback) {
  auto it = sync_metadata.find(key.ToString());
  if (it == sync_metadata.end()) {
    async::PostTask(async_, [callback = std::move(callback)] {
      callback(storage::Status::NOT_FOUND, "");
    });
    return;
  }
  async::PostTask(
      async_,
      [callback = std::move(callback), metadata = it->second] {
        callback(storage::Status::OK, metadata);
  });
}

}  // namespace cloud_sync
