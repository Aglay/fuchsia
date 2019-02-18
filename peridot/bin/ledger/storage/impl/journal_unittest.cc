// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/storage/impl/journal_impl.h"

#include <memory>
#include <vector>

#include <lib/callback/set_when_called.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "peridot/bin/ledger/encryption/fake/fake_encryption_service.h"
#include "peridot/bin/ledger/storage/fake/fake_db.h"
#include "peridot/bin/ledger/storage/impl/storage_test_utils.h"
#include "peridot/bin/ledger/storage/public/constants.h"
#include "peridot/bin/ledger/testing/test_with_environment.h"

namespace storage {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;

class JournalTest : public ledger::TestWithEnvironment {
 public:
  JournalTest()
      : encryption_service_(dispatcher()),
        page_storage_(&environment_, &encryption_service_,
                      std::make_unique<storage::fake::FakeDb>(dispatcher()),
                      "page_id"),
        object_identifier_(0u, 0u, MakeObjectDigest("value")) {}

  ~JournalTest() override {}

  // Test:
  void SetUp() override {
    Status status;
    bool called;
    page_storage_.Init(
        callback::Capture(callback::SetWhenCalled(&called), &status));
    RunLoopUntilIdle();
    ASSERT_TRUE(called);
    ASSERT_EQ(Status::OK, status);
  }

  // Casts the given |Journal| to |JournalImpl| and updates |journal_| to have
  // this value.
  void SetJournal(std::unique_ptr<Journal> journal) {
    journal_ = std::unique_ptr<JournalImpl>(
        static_cast<JournalImpl*>(journal.release()));
  }

  std::vector<Entry> GetCommitContents(const Commit& commit) {
    bool called;
    Status status;
    std::vector<Entry> result;
    auto on_next = [&result](Entry e) {
      result.push_back(e);
      return true;
    };
    page_storage_.GetCommitContents(
        commit, "", std::move(on_next),
        callback::Capture(callback::SetWhenCalled(&called), &status));
    RunLoopUntilIdle();
    EXPECT_TRUE(called);
    EXPECT_EQ(Status::OK, status);
    return result;
  }

 protected:
  encryption::FakeEncryptionService encryption_service_;
  PageStorageImpl page_storage_;

  ObjectIdentifier object_identifier_;

  std::unique_ptr<JournalImpl> journal_;

  FXL_DISALLOW_COPY_AND_ASSIGN(JournalTest);
};

TEST_F(JournalTest, ImplicitJournalsCommitEmptyJournal) {
  SetJournal(JournalImpl::Simple(JournalType::IMPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  bool called;
  Status status;
  std::unique_ptr<const Commit> commit;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);
  // If the journal's result is a no-op, we should not create a new commit, but
  // rather return the base commit.
  EXPECT_EQ(kFirstPageCommitId, commit->GetId());
}

TEST_F(JournalTest, ImplicitJournalsPutDeleteCommit) {
  SetJournal(JournalImpl::Simple(JournalType::IMPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  journal_->Put("key", object_identifier_, KeyPriority::EAGER);

  bool called;
  Status status;
  std::unique_ptr<const Commit> commit;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  std::vector<Entry> entries = GetCommitContents(*commit);
  ASSERT_THAT(entries, SizeIs(1));
  EXPECT_EQ("key", entries[0].key);
  EXPECT_EQ(object_identifier_, entries[0].object_identifier);
  EXPECT_EQ(KeyPriority::EAGER, entries[0].priority);

  // Ledger's content is now a single entry "key" -> "value". Delete it.
  SetJournal(JournalImpl::Simple(JournalType::IMPLICIT, &environment_,
                                 &page_storage_, commit->GetId()));
  journal_->Delete("key");

  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  ASSERT_THAT(GetCommitContents(*commit), ElementsAre());
}

TEST_F(JournalTest, ImplicitJournalsPutRollback) {
  SetJournal(JournalImpl::Simple(JournalType::IMPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  journal_->Put("key", object_identifier_, KeyPriority::EAGER);

  bool called;
  Status status;
  journal_->Rollback(
      callback::Capture(callback::SetWhenCalled(&called), &status));

  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);

  std::vector<CommitId> heads;
  page_storage_.GetHeadCommitIds(
      callback::Capture(callback::SetWhenCalled(&called), &status, &heads));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_THAT(heads, SizeIs(1));
  EXPECT_EQ(kFirstPageCommitId, heads[0]);
}

TEST_F(JournalTest, ExplicitJournalsSinglePut) {
  SetJournal(JournalImpl::Simple(JournalType::EXPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  journal_->Put("key", object_identifier_, KeyPriority::EAGER);

  bool called;
  Status status;
  std::unique_ptr<const Commit> commit;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));

  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  std::vector<Entry> entries = GetCommitContents(*commit);
  ASSERT_THAT(entries, SizeIs(1));
  EXPECT_EQ("key", entries[0].key);
  EXPECT_EQ(object_identifier_, entries[0].object_identifier);
  EXPECT_EQ(KeyPriority::EAGER, entries[0].priority);
}

TEST_F(JournalTest, ExplicitJournalsMultiplePutsDeletes) {
  int size = 3;
  SetJournal(JournalImpl::Simple(JournalType::EXPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  bool called;
  Status status;
  // Insert keys {"0", "1", "2"}. Also insert key "0" a second time, with a
  // different value, and delete a non-existing key.
  for (int i = 0; i < size; i++) {
    journal_->Put(std::to_string(i), object_identifier_, KeyPriority::EAGER);
  }
  journal_->Delete("notfound");

  ObjectIdentifier object_identifier_2(0u, 0u,
                                       MakeObjectDigest("another value"));
  journal_->Put("0", object_identifier_2, KeyPriority::EAGER);

  std::unique_ptr<const Commit> commit;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));

  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  std::vector<Entry> entries = GetCommitContents(*commit);
  ASSERT_THAT(entries, SizeIs(size));
  for (int i = 0; i < size; i++) {
    EXPECT_EQ(std::to_string(i), entries[i].key);
    if (i == 0) {
      EXPECT_EQ(object_identifier_2, entries[i].object_identifier);
    } else {
      EXPECT_EQ(object_identifier_, entries[i].object_identifier);
    }
    EXPECT_EQ(KeyPriority::EAGER, entries[i].priority);
  }

  // Delete keys {"0", "2"}. Also insert a key, that is deleted on the same
  // journal.
  SetJournal(JournalImpl::Simple(JournalType::EXPLICIT, &environment_,
                                 &page_storage_, commit->GetId()));
  journal_->Delete("0");
  journal_->Delete("2");
  journal_->Put("tmp", object_identifier_, KeyPriority::EAGER);
  journal_->Delete("tmp");

  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));

  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  // Check that there is only one entry left.
  entries = GetCommitContents(*commit);
  ASSERT_THAT(entries, SizeIs(1));
  EXPECT_EQ("1", entries[0].key);
  EXPECT_EQ(object_identifier_, entries[0].object_identifier);
  EXPECT_EQ(KeyPriority::EAGER, entries[0].priority);
}

TEST_F(JournalTest, ExplicitJournalsClear) {
  int size = 3;
  SetJournal(JournalImpl::Simple(JournalType::EXPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  bool called;
  Status status;
  // Insert keys {"0", "1", "2"}.
  for (int i = 0; i < size; i++) {
    journal_->Put(std::to_string(i), object_identifier_, KeyPriority::EAGER);
  }

  std::unique_ptr<const Commit> commit;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));

  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  ASSERT_THAT(GetCommitContents(*commit), SizeIs(size));

  // Clear the contents.
  SetJournal(JournalImpl::Simple(JournalType::EXPLICIT, &environment_,
                                 &page_storage_, commit->GetId()));
  journal_->Clear();

  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit);

  EXPECT_THAT(GetCommitContents(*commit), IsEmpty());
}

TEST_F(JournalTest, MergeJournal) {
  // Create 2 commits from the |kFirstPageCommitId|, one implicit with a key
  // "0", and one explicit with a key "1".
  SetJournal(JournalImpl::Simple(JournalType::IMPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  journal_->Put("0", object_identifier_, KeyPriority::EAGER);

  bool called;
  Status status;
  std::unique_ptr<const Commit> commit_0;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit_0));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit_0);

  SetJournal(JournalImpl::Simple(JournalType::EXPLICIT, &environment_,
                                 &page_storage_,
                                 kFirstPageCommitId.ToString()));
  journal_->Put("1", object_identifier_, KeyPriority::EAGER);

  std::unique_ptr<const Commit> commit_1;
  journal_->Commit(
      callback::Capture(callback::SetWhenCalled(&called), &status, &commit_1));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, commit_1);

  // Create a merge journal, adding only a key "2".
  SetJournal(JournalImpl::Merge(&environment_, &page_storage_,
                                commit_0->GetId(), commit_1->GetId()));
  journal_->Put("2", object_identifier_, KeyPriority::EAGER);

  std::unique_ptr<const Commit> merge_commit;
  journal_->Commit(callback::Capture(callback::SetWhenCalled(&called), &status,
                                     &merge_commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_EQ(Status::OK, status);
  ASSERT_NE(nullptr, merge_commit);

  // Expect the contents to have two keys: "0" and "2".
  std::vector<Entry> entries = GetCommitContents(*merge_commit);
  entries = GetCommitContents(*merge_commit);
  ASSERT_THAT(entries, SizeIs(2));
  EXPECT_EQ("0", entries[0].key);
  EXPECT_EQ(object_identifier_, entries[0].object_identifier);
  EXPECT_EQ(KeyPriority::EAGER, entries[0].priority);

  EXPECT_EQ("2", entries[1].key);
  EXPECT_EQ(object_identifier_, entries[1].object_identifier);
  EXPECT_EQ(KeyPriority::EAGER, entries[1].priority);
}

}  // namespace
}  // namespace storage
