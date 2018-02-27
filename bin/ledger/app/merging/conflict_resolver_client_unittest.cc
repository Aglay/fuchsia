// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/merging/merge_resolver.h"

#include <memory>
#include <string>

#include "garnet/lib/gtest/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/app/constants.h"
#include "peridot/bin/ledger/app/merging/custom_merge_strategy.h"
#include "peridot/bin/ledger/app/merging/test_utils.h"
#include "peridot/bin/ledger/coroutine/coroutine_impl.h"
#include "peridot/bin/ledger/encryption/primitives/hash.h"
#include "peridot/bin/ledger/storage/impl/page_storage_impl.h"
#include "peridot/bin/ledger/storage/public/constants.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"
#include "peridot/lib/callback/cancellable_helper.h"
#include "peridot/lib/callback/capture.h"
#include "peridot/lib/callback/set_when_called.h"

namespace ledger {
namespace {
class ConflictResolverClientTest : public test::TestWithPageStorage {
 public:
  ConflictResolverClientTest()
      : environment_(message_loop_.task_runner(), message_loop_.task_runner()) {
  }
  ~ConflictResolverClientTest() override {}

 protected:
  storage::PageStorage* page_storage() override { return page_storage_; }

  void SetUp() override {
    ::testing::Test::SetUp();
    std::unique_ptr<storage::PageStorage> page_storage;
    ASSERT_TRUE(CreatePageStorage(&page_storage));
    page_storage_ = page_storage.get();

    std::unique_ptr<MergeResolver> resolver = std::make_unique<MergeResolver>(
        [] {}, &environment_, page_storage_,
        std::make_unique<test::TestBackoff>(nullptr));
    resolver->SetMergeStrategy(nullptr);
    resolver->set_on_empty(MakeQuitTask());
    merge_resolver_ = resolver.get();

    page_manager_ = std::make_unique<PageManager>(
        &environment_, std::move(page_storage), nullptr, std::move(resolver),
        PageManager::PageStorageState::NEW);
  }

  storage::CommitId CreateCommit(
      storage::CommitIdView parent_id,
      std::function<void(storage::Journal*)> contents) {
    storage::Status status;
    bool called;
    std::unique_ptr<storage::Journal> journal;
    page_storage_->StartCommit(
        parent_id.ToString(), storage::JournalType::IMPLICIT,
        callback::Capture(callback::SetWhenCalled(&called), &status, &journal));
    RunLoopUntilIdle();
    EXPECT_TRUE(called);
    EXPECT_EQ(storage::Status::OK, status);

    contents(journal.get());
    std::unique_ptr<const storage::Commit> commit;
    page_storage_->CommitJournal(
        std::move(journal),
        callback::Capture(callback::SetWhenCalled(&called), &status, &commit));
    RunLoopUntilIdle();
    EXPECT_TRUE(called);
    EXPECT_EQ(storage::Status::OK, status);
    return commit->GetId();
  }

  storage::PageStorage* page_storage_;
  MergeResolver* merge_resolver_;

 private:
  Environment environment_;
  std::unique_ptr<PageManager> page_manager_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ConflictResolverClientTest);
};

class ConflictResolverImpl : public ConflictResolver {
 public:
  explicit ConflictResolverImpl(
      f1dl::InterfaceRequest<ConflictResolver> request)
      : binding_(this, std::move(request)) {
    binding_.set_error_handler([this] { this->disconnected = true; });
  }
  ~ConflictResolverImpl() override {}

  struct ResolveRequest {
    f1dl::InterfaceHandle<PageSnapshot> left_version;
    f1dl::InterfaceHandle<PageSnapshot> right_version;
    f1dl::InterfaceHandle<PageSnapshot> common_version;
    MergeResultProviderPtr result_provider_ptr;
    bool result_provider_disconnected = false;

    ResolveRequest(f1dl::InterfaceHandle<PageSnapshot> left_version,
                   f1dl::InterfaceHandle<PageSnapshot> right_version,
                   f1dl::InterfaceHandle<PageSnapshot> common_version,
                   f1dl::InterfaceHandle<MergeResultProvider> result_provider)
        : left_version(std::move(left_version)),
          right_version(std::move(right_version)),
          common_version(std::move(common_version)),
          result_provider_ptr(result_provider.Bind()) {
      result_provider_ptr.set_error_handler(
          [this] { result_provider_disconnected = true; });
    }
  };

  std::vector<ResolveRequest> requests;
  bool disconnected = false;

 private:
  // ConflictResolver:
  void Resolve(
      f1dl::InterfaceHandle<PageSnapshot> left_version,
      f1dl::InterfaceHandle<PageSnapshot> right_version,
      f1dl::InterfaceHandle<PageSnapshot> common_version,
      f1dl::InterfaceHandle<MergeResultProvider> result_provider) override {
    requests.emplace_back(std::move(left_version), std::move(right_version),
                          std::move(common_version),
                          std::move(result_provider));
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  }

  f1dl::Binding<ConflictResolver> binding_;
};

TEST_F(ConflictResolverClientTest, Error) {
  // Set up conflict.
  CreateCommit(storage::kFirstPageCommitId,
               AddKeyValueToJournal("key1", "value1"));
  CreateCommit(storage::kFirstPageCommitId,
               AddKeyValueToJournal("key2", "value2"));

  // Set the resolver.
  ConflictResolverPtr conflict_resolver_ptr;
  ConflictResolverImpl conflict_resolver_impl(
      conflict_resolver_ptr.NewRequest());
  std::unique_ptr<CustomMergeStrategy> custom_merge_strategy =
      std::make_unique<CustomMergeStrategy>(std::move(conflict_resolver_ptr));

  bool custom_strategy_error = false;
  custom_merge_strategy->SetOnError([&custom_strategy_error]() {
    custom_strategy_error = true;
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  });

  merge_resolver_->SetMergeStrategy(std::move(custom_merge_strategy));
  bool called;
  storage::Status status;
  std::vector<storage::CommitId> ids;
  page_storage_->GetHeadCommitIds(
      callback::Capture(callback::SetWhenCalled(&called), &status, &ids));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, status);
  EXPECT_EQ(2u, ids.size());

  EXPECT_FALSE(merge_resolver_->IsEmpty());
  EXPECT_EQ(1u, conflict_resolver_impl.requests.size());

  // Create a bogus conflict resolution.
  f1dl::Array<MergedValuePtr> merged_values =
      f1dl::Array<MergedValuePtr>::New(0);
  {
    MergedValuePtr merged_value = MergedValue::New();
    merged_value->key = convert::ToArray("unknown_key");
    merged_value->source = ValueSource::RIGHT;
    merged_values.push_back(std::move(merged_value));
  }

  Status merge_status;
  conflict_resolver_impl.requests[0].result_provider_ptr->Merge(
      std::move(merged_values),
      callback::Capture(callback::SetWhenCalled(&called), &merge_status));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(Status::KEY_NOT_FOUND, merge_status);

  EXPECT_TRUE(conflict_resolver_impl.requests[0].result_provider_disconnected);
  EXPECT_EQ(2u, conflict_resolver_impl.requests.size());
}

TEST_F(ConflictResolverClientTest, MergeNonConflicting) {
  // Set up conflict.
  CreateCommit(storage::kFirstPageCommitId,
               AddKeyValueToJournal("key1", "value1"));
  CreateCommit(storage::kFirstPageCommitId,
               AddKeyValueToJournal("key2", "value2"));

  // Set the resolver.
  ConflictResolverPtr conflict_resolver_ptr;
  ConflictResolverImpl conflict_resolver_impl(
      conflict_resolver_ptr.NewRequest());
  std::unique_ptr<CustomMergeStrategy> custom_merge_strategy =
      std::make_unique<CustomMergeStrategy>(std::move(conflict_resolver_ptr));

  merge_resolver_->SetMergeStrategy(std::move(custom_merge_strategy));

  RunLoopUntilIdle();

  EXPECT_FALSE(merge_resolver_->IsEmpty());
  EXPECT_EQ(1u, conflict_resolver_impl.requests.size());

  bool called;
  Status status;
  conflict_resolver_impl.requests[0]
      .result_provider_ptr->MergeNonConflictingEntries(
          callback::Capture(callback::SetWhenCalled(&called), &status));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(Status::OK, status);

  conflict_resolver_impl.requests[0].result_provider_ptr->Done(
      callback::Capture(callback::SetWhenCalled(&called), &status));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(Status::OK, status);

  storage::Status storage_status;
  std::vector<storage::CommitId> ids;
  page_storage_->GetHeadCommitIds(callback::Capture(
      callback::SetWhenCalled(&called), &storage_status, &ids));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);
  // The merge happened.
  EXPECT_EQ(1u, ids.size());

  // Let's verify the contents.
  std::unique_ptr<const storage::Commit> commit;
  page_storage_->GetCommit(ids[0],
                           callback::Capture(callback::SetWhenCalled(&called),
                                             &storage_status, &commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);

  storage::Entry key1_entry, key2_entry;
  page_storage_->GetEntryFromCommit(
      *commit, "key1",
      callback::Capture(callback::SetWhenCalled(&called), &storage_status,
                        &key1_entry));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);

  page_storage_->GetEntryFromCommit(
      *commit, "key2",
      callback::Capture(callback::SetWhenCalled(&called), &storage_status,
                        &key2_entry));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);

  std::string value1, value2;
  GetValue(key1_entry.object_identifier, &value1);
  EXPECT_EQ("value1", value1);
  GetValue(key2_entry.object_identifier, &value2);
  EXPECT_EQ("value2", value2);
}

TEST_F(ConflictResolverClientTest, MergeNonConflictingOrdering) {
  // Set up conflict.
  storage::CommitId base_id = CreateCommit(
      storage::kFirstPageCommitId, AddKeyValueToJournal("key1", "value1"));
  CreateCommit(base_id, AddKeyValueToJournal("key2", "value2"));
  CreateCommit(base_id, AddKeyValueToJournal("key1", "value1bis"));

  // Set the resolver.
  ConflictResolverPtr conflict_resolver_ptr;
  ConflictResolverImpl conflict_resolver_impl(
      conflict_resolver_ptr.NewRequest());
  std::unique_ptr<CustomMergeStrategy> custom_merge_strategy =
      std::make_unique<CustomMergeStrategy>(std::move(conflict_resolver_ptr));

  merge_resolver_->SetMergeStrategy(std::move(custom_merge_strategy));

  RunLoopUntilIdle();

  EXPECT_FALSE(merge_resolver_->IsEmpty());
  EXPECT_EQ(1u, conflict_resolver_impl.requests.size());

  Status status;
  f1dl::Array<MergedValuePtr> merged_values =
      f1dl::Array<MergedValuePtr>::New(0);
  {
    MergedValuePtr merged_value = MergedValue::New();
    merged_value->key = convert::ToArray("key1");
    merged_value->source = ValueSource::RIGHT;
    merged_values.push_back(std::move(merged_value));
  }

  bool called;
  conflict_resolver_impl.requests[0].result_provider_ptr->Merge(
      std::move(merged_values),
      callback::Capture(callback::SetWhenCalled(&called), &status));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(Status::OK, status);

  conflict_resolver_impl.requests[0]
      .result_provider_ptr->MergeNonConflictingEntries(
          callback::Capture(callback::SetWhenCalled(&called), &status));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(Status::OK, status);

  conflict_resolver_impl.requests[0].result_provider_ptr->Done(
      callback::Capture(callback::SetWhenCalled(&called), &status));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(Status::OK, status);

  storage::Status storage_status;
  std::vector<storage::CommitId> ids;
  page_storage_->GetHeadCommitIds(callback::Capture(
      callback::SetWhenCalled(&called), &storage_status, &ids));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);
  // The merge happened.
  EXPECT_EQ(1u, ids.size());

  // Let's verify the contents.
  std::unique_ptr<const storage::Commit> commit;
  page_storage_->GetCommit(ids[0],
                           callback::Capture(callback::SetWhenCalled(&called),
                                             &storage_status, &commit));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);

  storage::Entry key1_entry, key2_entry;
  page_storage_->GetEntryFromCommit(
      *commit, "key1",
      callback::Capture(callback::SetWhenCalled(&called), &storage_status,
                        &key1_entry));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);

  page_storage_->GetEntryFromCommit(
      *commit, "key2",
      callback::Capture(callback::SetWhenCalled(&called), &storage_status,
                        &key2_entry));
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_EQ(storage::Status::OK, storage_status);

  std::string value1, value2;
  GetValue(key1_entry.object_identifier, &value1);
  EXPECT_EQ("value1bis", value1);
  GetValue(key2_entry.object_identifier, &value2);
  EXPECT_EQ("value2", value2);
}

}  // namespace
}  // namespace ledger
