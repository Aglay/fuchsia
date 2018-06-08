// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_FAKE_FAKE_PAGE_STORAGE_H_
#define PERIDOT_BIN_LEDGER_STORAGE_FAKE_FAKE_PAGE_STORAGE_H_

#include <lib/async/dispatcher.h>

#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "lib/fxl/functional/closure.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/strings/string_view.h"
#include "peridot/bin/ledger/encryption/fake/fake_encryption_service.h"
#include "peridot/bin/ledger/storage/fake/fake_journal_delegate.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"
#include "peridot/bin/ledger/storage/testing/page_storage_empty_impl.h"

namespace storage {
namespace fake {

class FakePageStorage : public PageStorageEmptyImpl {
 public:
  explicit FakePageStorage(PageId page_id);
  FakePageStorage(async_t* async, PageId page_id);
  ~FakePageStorage() override;

  // PageStorage:
  PageId GetId() override;
  void GetHeadCommitIds(
      std::function<void(Status, std::vector<CommitId>)> callback) override;
  void GetCommit(CommitIdView commit_id,
                 std::function<void(Status, std::unique_ptr<const Commit>)>
                     callback) override;
  void StartCommit(
      const CommitId& commit_id, JournalType journal_type,
      std::function<void(Status, std::unique_ptr<Journal>)> callback) override;
  void StartMergeCommit(
      const CommitId& left, const CommitId& right,
      std::function<void(Status, std::unique_ptr<Journal>)> callback) override;
  void CommitJournal(
      std::unique_ptr<Journal> journal,
      std::function<void(Status, std::unique_ptr<const storage::Commit>)>
          callback) override;
  void RollbackJournal(std::unique_ptr<Journal> journal,
                       std::function<void(Status)> callback) override;
  Status AddCommitWatcher(CommitWatcher* watcher) override;
  Status RemoveCommitWatcher(CommitWatcher* watcher) override;
  void AddObjectFromLocal(
      std::unique_ptr<DataSource> data_source,
      std::function<void(Status, ObjectIdentifier)> callback) override;
  void GetObject(ObjectIdentifier object_identifier, Location location,
                 std::function<void(Status, std::unique_ptr<const Object>)>
                     callback) override;
  void GetPiece(ObjectIdentifier object_identifier,
                std::function<void(Status, std::unique_ptr<const Object>)>
                    callback) override;
  void GetCommitContents(const Commit& commit, std::string min_key,
                         std::function<bool(Entry)> on_next,
                         std::function<void(Status)> on_done) override;
  void GetEntryFromCommit(const Commit& commit, std::string key,
                          std::function<void(Status, Entry)> callback) override;

  // For testing:
  void set_autocommit(bool autocommit) { autocommit_ = autocommit; }
  const std::map<std::string, std::unique_ptr<FakeJournalDelegate>>&
  GetJournals() const;
  const std::map<ObjectIdentifier, std::string>& GetObjects() const;
  // Deletes this object from the fake local storage, but keeps it in its
  // "network" storage.
  void DeleteObjectFromLocal(const ObjectIdentifier& object_identifier);
  // If set to true, no commit notification is sent to the commit watchers.
  void SetDropCommitNotifications(bool drop);

 private:
  void SendNextObject();

  bool autocommit_ = true;
  bool drop_commit_notifications_ = false;

  std::default_random_engine rng_;
  std::map<std::string, std::unique_ptr<FakeJournalDelegate>> journals_;
  std::map<ObjectIdentifier, std::string> objects_;
  std::set<CommitId> heads_;
  std::set<CommitWatcher*> watchers_;
  std::vector<fxl::Closure> object_requests_;
  async_t* const async_;
  PageId page_id_;
  encryption::FakeEncryptionService encryption_service_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakePageStorage);
};

}  // namespace fake
}  // namespace storage

#endif  // PERIDOT_BIN_LEDGER_STORAGE_FAKE_FAKE_PAGE_STORAGE_H_
