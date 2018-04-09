// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/page_impl.h"

#include <algorithm>
#include <map>
#include <memory>

#include "garnet/lib/backoff/exponential_backoff.h"
#include "garnet/lib/callback/capture.h"
#include "garnet/lib/callback/set_when_called.h"
#include "garnet/lib/gtest/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/clone.h"
#include "lib/fsl/socket/strings.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/strings/string_printf.h"
#include "peridot/bin/ledger/app/constants.h"
#include "peridot/bin/ledger/app/fidl/serialization_size.h"
#include "peridot/bin/ledger/app/merging/merge_resolver.h"
#include "peridot/bin/ledger/app/page_manager.h"
#include "peridot/bin/ledger/coroutine/coroutine_impl.h"
#include "peridot/bin/ledger/environment/environment.h"
#include "peridot/bin/ledger/storage/fake/fake_journal.h"
#include "peridot/bin/ledger/storage/fake/fake_journal_delegate.h"
#include "peridot/bin/ledger/storage/fake/fake_page_storage.h"
#include "peridot/lib/convert/convert.h"

namespace ledger {
namespace {
std::string ToString(const mem::BufferPtr& vmo) {
  std::string value;
  bool status = fsl::StringFromVmo(*vmo, &value);
  FXL_DCHECK(status);
  return value;
}

class PageImplTest : public gtest::TestWithMessageLoop {
 public:
  PageImplTest()
      : environment_(message_loop_.task_runner(), message_loop_.async()) {}
  ~PageImplTest() override {}

 protected:
  // ApplicationTestBase:
  void SetUp() override {
    ::testing::Test::SetUp();
    page_id1_ = storage::PageId(kPageIdSize, 'a');
    auto fake_storage =
        std::make_unique<storage::fake::FakePageStorage>(page_id1_);
    fake_storage_ = fake_storage.get();
    auto resolver = std::make_unique<MergeResolver>(
        [] {}, &environment_, fake_storage_,
        std::make_unique<backoff::ExponentialBackoff>(
            fxl::TimeDelta::FromSeconds(0), 1u,
            fxl::TimeDelta::FromSeconds(0)));
    resolver_ = resolver.get();

    manager_ = std::make_unique<PageManager>(
        &environment_, std::move(fake_storage), nullptr, std::move(resolver),
        PageManager::PageStorageState::NEEDS_SYNC);
    Status status;
    manager_->BindPage(page_ptr_.NewRequest(),
                       callback::Capture(MakeQuitTask(), &status));
    EXPECT_EQ(Status::OK, status);
    RunLoop();
  }

  void CommitFirstPendingJournal(
      const std::map<std::string,
                     std::unique_ptr<storage::fake::FakeJournalDelegate>>&
          journals) {
    for (const auto& journal_pair : journals) {
      const auto& journal = journal_pair.second;
      if (!journal->IsCommitted() && !journal->IsRolledBack()) {
        journal->ResolvePendingCommit(storage::Status::OK);
        return;
      }
    }
  }

  storage::ObjectIdentifier AddObjectToStorage(std::string value_string) {
    storage::Status status;
    storage::ObjectIdentifier object_identifier;
    fake_storage_->AddObjectFromLocal(
        storage::DataSource::Create(std::move(value_string)),
        callback::Capture(MakeQuitTask(), &status, &object_identifier));
    RunLoop();
    EXPECT_EQ(storage::Status::OK, status);
    return object_identifier;
  }

  std::unique_ptr<const storage::Object> AddObject(const std::string& value) {
    storage::ObjectIdentifier object_identifier = AddObjectToStorage(value);

    storage::Status status;
    std::unique_ptr<const storage::Object> object;
    fake_storage_->GetObject(
        object_identifier, storage::PageStorage::Location::LOCAL,
        callback::Capture(MakeQuitTask(), &status, &object));
    RunLoop();
    EXPECT_EQ(storage::Status::OK, status);
    return object;
  }

  std::string GetKey(size_t index, size_t min_key_size = 0u) {
    std::string result = fxl::StringPrintf("key %04" PRIuMAX, index);
    result.resize(std::max(result.size(), min_key_size));
    return result;
  }

  std::string GetValue(size_t index, size_t min_value_size = 0u) {
    std::string result = fxl::StringPrintf("val %zu", index);
    result.resize(std::max(result.size(), min_value_size));
    return result;
  }

  void AddEntries(int entry_count,
                  size_t min_key_size = 0u,
                  size_t min_value_size = 0u) {
    FXL_DCHECK(entry_count <= 10000);
    auto callback_statusok = [this](Status status) {
      EXPECT_EQ(Status::OK, status);
      message_loop_.PostQuitTask();
    };
    page_ptr_->StartTransaction(callback_statusok);
    RunLoop();

    for (int i = 0; i < entry_count; ++i) {
      page_ptr_->Put(convert::ToArray(GetKey(i, min_key_size)),
                     convert::ToArray(GetValue(i, min_value_size)),
                     callback_statusok);
      RunLoop();
    }
    page_ptr_->Commit(callback_statusok);
    RunLoop();
  }

  PageSnapshotPtr GetSnapshot(fidl::VectorPtr<uint8_t> prefix = nullptr) {
    auto callback_getsnapshot = [this](Status status) {
      EXPECT_EQ(Status::OK, status);
      message_loop_.PostQuitTask();
    };
    PageSnapshotPtr snapshot;
    page_ptr_->GetSnapshot(snapshot.NewRequest(), std::move(prefix), nullptr,
                           callback_getsnapshot);
    RunLoop();
    return snapshot;
  }

  storage::PageId page_id1_;
  storage::fake::FakePageStorage* fake_storage_;
  std::unique_ptr<PageManager> manager_;
  MergeResolver* resolver_;

  PagePtr page_ptr_;
  Environment environment_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageImplTest);
};

TEST_F(PageImplTest, GetId) {
  page_ptr_->GetId([this](ledger::PageId page_id) {
    EXPECT_EQ(page_id1_, convert::ToString(page_id.id));
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, PutNoTransaction) {
  std::string key("some_key");
  std::string value("a small value");
  auto callback = [this, &key, &value](Status status) {
    EXPECT_EQ(Status::OK, status);
    auto objects = fake_storage_->GetObjects();
    EXPECT_EQ(1u, objects.size());
    storage::ObjectIdentifier object_identifier = objects.begin()->first;
    std::string actual_value = objects.begin()->second;
    EXPECT_EQ(value, actual_value);

    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_TRUE(it->second->IsCommitted());
    EXPECT_EQ(1u, it->second->GetData().size());
    storage::fake::FakeJournalDelegate::Entry entry =
        it->second->GetData().at(key);
    EXPECT_EQ(object_identifier, entry.value);
    EXPECT_FALSE(entry.deleted);
    EXPECT_EQ(storage::KeyPriority::EAGER, entry.priority);
    message_loop_.PostQuitTask();
  };
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value), callback);
  RunLoop();
}

TEST_F(PageImplTest, PutReferenceNoTransaction) {
  std::string object_data("some_data");
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromString(object_data, &vmo));

  Status status;
  ReferencePtr reference;
  page_ptr_->CreateReferenceFromVmo(
      std::move(vmo).ToTransport(),
      callback::Capture(MakeQuitTask(), &status, &reference));
  RunLoop();

  ASSERT_EQ(Status::OK, status);

  std::string key("some_key");
  page_ptr_->PutReference(convert::ToArray(key), std::move(*reference),
                          Priority::LAZY,
                          callback::Capture(MakeQuitTask(), &status));
  RunLoop();

  EXPECT_EQ(Status::OK, status);
  auto objects = fake_storage_->GetObjects();
  // No object should have been added.
  EXPECT_EQ(1u, objects.size());

  const std::map<std::string,
                 std::unique_ptr<storage::fake::FakeJournalDelegate>>&
      journals = fake_storage_->GetJournals();
  EXPECT_EQ(1u, journals.size());
  auto it = journals.begin();
  EXPECT_TRUE(it->second->IsCommitted());
  EXPECT_EQ(1u, it->second->GetData().size());
  storage::fake::FakeJournalDelegate::Entry entry =
      it->second->GetData().at(key);
  std::unique_ptr<const storage::Object> object = AddObject(object_data);
  EXPECT_EQ(object->GetIdentifier().object_digest, entry.value.object_digest);
  EXPECT_FALSE(entry.deleted);
  EXPECT_EQ(storage::KeyPriority::LAZY, entry.priority);
}

TEST_F(PageImplTest, PutUnknownReference) {
  std::string key("some_key");
  ReferencePtr reference = Reference::New();
  reference->opaque_id = convert::ToArray("12345678");

  auto callback = [this](Status status) {
    EXPECT_EQ(Status::REFERENCE_NOT_FOUND, status);
    auto objects = fake_storage_->GetObjects();
    // No object should have been added.
    EXPECT_EQ(0u, objects.size());

    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(0u, journals.size());
    message_loop_.PostQuitTask();
  };
  page_ptr_->PutReference(convert::ToArray(key), std::move(*reference),
                          Priority::LAZY, callback);
  RunLoop();
}

TEST_F(PageImplTest, PutKeyTooLarge) {
  std::string value("a small value");

  zx::channel writer, reader;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &writer, &reader));
  page_ptr_.Bind(std::move(writer));

  // Key too large; message doesn't go through, failing on validation.
  const size_t key_size = kMaxKeySize + 1;
  std::string key = GetKey(1, key_size);
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value),
                 [](Status status) {});
  zx_status_t status = reader.read(0, nullptr, 0, nullptr, nullptr, 0, nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(ZX_ERR_SHOULD_WAIT, status);

  // With a smaller key, message goes through.
  key = GetKey(1, kMaxKeySize);
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value),
                 [](Status status) {});
  status = reader.read(0, nullptr, 0, nullptr, nullptr, 0, nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL, status);
}

TEST_F(PageImplTest, PutReferenceKeyTooLarge) {
  std::string object_data("some_data");
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromString(object_data, &vmo));

  Status reference_status;
  ReferencePtr reference;
  page_ptr_->CreateReferenceFromVmo(
      std::move(vmo).ToTransport(),
      callback::Capture(MakeQuitTask(), &reference_status, &reference));
  RunLoop();
  ASSERT_EQ(Status::OK, reference_status);

  zx::channel writer, reader;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &writer, &reader));
  page_ptr_.Bind(std::move(writer));

  // Key too large; message doesn't go through, failing on validation.
  const size_t key_size = kMaxKeySize + 1;
  std::string key = GetKey(1, key_size);
  page_ptr_->PutReference(convert::ToArray(key), fidl::Clone(*reference),
                          Priority::EAGER, [](Status status) {});
  zx_status_t status = reader.read(0, nullptr, 0, nullptr, nullptr, 0, nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(ZX_ERR_SHOULD_WAIT, status);

  // With a smaller key, message goes through.
  key = GetKey(1, kMaxKeySize);
  page_ptr_->PutReference(convert::ToArray(key), std::move(*reference),
                          Priority::EAGER, [](Status status) {});
  status = reader.read(0, nullptr, 0, nullptr, nullptr, 0, nullptr);
  RunLoopUntilIdle();
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL, status);
}

TEST_F(PageImplTest, DeleteNoTransaction) {
  std::string key("some_key");

  page_ptr_->Delete(convert::ToArray(key), [this, &key](Status status) {
    EXPECT_EQ(Status::OK, status);
    auto objects = fake_storage_->GetObjects();
    // No object should have been added.
    EXPECT_EQ(0u, objects.size());

    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_TRUE(it->second->IsCommitted());
    EXPECT_EQ(1u, it->second->GetData().size());
    storage::fake::FakeJournalDelegate::Entry entry =
        it->second->GetData().at(key);
    EXPECT_TRUE(entry.deleted);
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, TransactionCommit) {
  std::string key1("some_key1");
  storage::ObjectDigest object_digest1;
  std::string value("a small value");

  std::string key2("some_key2");
  std::string value2("another value");

  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromString(value2, &vmo));

  Status status;
  ReferencePtr reference;
  page_ptr_->CreateReferenceFromVmo(
      std::move(vmo).ToTransport(),
      callback::Capture(MakeQuitTask(), &status, &reference));
  RunLoop();
  ASSERT_EQ(Status::OK, status);

  // Sequence of operations:
  //  - StartTransaction
  //  - Put
  //  - PutReference
  //  - Delete
  //  - Commit
  page_ptr_->StartTransaction(callback::Capture(MakeQuitTask(), &status));
  RunLoop();
  EXPECT_EQ(Status::OK, status);

  page_ptr_->Put(convert::ToArray(key1), convert::ToArray(value),
                 callback::Capture(MakeQuitTask(), &status));
  RunLoop();

  {
    EXPECT_EQ(Status::OK, status);
    auto objects = fake_storage_->GetObjects();
    EXPECT_EQ(2u, objects.size());
    // Objects are ordered by a randomly assigned object id, so we can't know
    // the correct possition of the value in the map.
    bool object_found = false;
    for (auto object : objects) {
      if (object.second == value) {
        object_found = true;
        object_digest1 = object.first.object_digest;
        break;
      }
    }
    EXPECT_TRUE(object_found);

    // No finished commit yet.
    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_FALSE(it->second->IsCommitted());
    EXPECT_EQ(1u, it->second->GetData().size());
    storage::fake::FakeJournalDelegate::Entry entry =
        it->second->GetData().at(key1);
    EXPECT_EQ(object_digest1, entry.value.object_digest);
    EXPECT_FALSE(entry.deleted);
    EXPECT_EQ(storage::KeyPriority::EAGER, entry.priority);
  }

  page_ptr_->PutReference(convert::ToArray(key2), std::move(*reference),
                          Priority::LAZY,
                          callback::Capture(MakeQuitTask(), &status));
  RunLoop();

  {
    EXPECT_EQ(Status::OK, status);
    EXPECT_EQ(2u, fake_storage_->GetObjects().size());

    // No finished commit yet, with now two entries.
    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_FALSE(it->second->IsCommitted());
    EXPECT_EQ(2u, it->second->GetData().size());
    storage::fake::FakeJournalDelegate::Entry entry =
        it->second->GetData().at(key2);
    EXPECT_EQ(AddObject(value2)->GetIdentifier().object_digest,
              entry.value.object_digest);
    EXPECT_FALSE(entry.deleted);
    EXPECT_EQ(storage::KeyPriority::LAZY, entry.priority);
  }

  auto delete_callback = [this, &key2](Status status) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_EQ(2u, fake_storage_->GetObjects().size());

    // No finished commit yet, with the second entry deleted.
    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_FALSE(it->second->IsCommitted());
    EXPECT_EQ(2u, it->second->GetData().size());
    storage::fake::FakeJournalDelegate::Entry entry =
        it->second->GetData().at(key2);
    EXPECT_TRUE(entry.deleted);
    message_loop_.PostQuitTask();
  };

  page_ptr_->Delete(convert::ToArray(key2), delete_callback);
  RunLoop();

  page_ptr_->Commit([this](Status status) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_EQ(2u, fake_storage_->GetObjects().size());

    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_TRUE(it->second->IsCommitted());
    EXPECT_EQ(2u, it->second->GetData().size());
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, TransactionRollback) {
  // Sequence of operations:
  //  - StartTransaction
  //  - Rollback
  page_ptr_->StartTransaction(
      [](Status status) { EXPECT_EQ(Status::OK, status); });
  page_ptr_->Rollback([this](Status status) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_EQ(0u, fake_storage_->GetObjects().size());

    // Only one journal, rollbacked.
    const std::map<std::string,
                   std::unique_ptr<storage::fake::FakeJournalDelegate>>&
        journals = fake_storage_->GetJournals();
    EXPECT_EQ(1u, journals.size());
    auto it = journals.begin();
    EXPECT_TRUE(it->second->IsRolledBack());
    EXPECT_EQ(0u, it->second->GetData().size());
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, NoTwoTransactions) {
  // Sequence of operations:
  //  - StartTransaction
  //  - StartTransaction
  page_ptr_->StartTransaction(
      [](Status status) { EXPECT_EQ(Status::OK, status); });
  page_ptr_->StartTransaction([this](Status status) {
    EXPECT_EQ(Status::TRANSACTION_ALREADY_IN_PROGRESS, status);
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, NoTransactionCommit) {
  // Sequence of operations:
  //  - Commit
  page_ptr_->Commit([this](Status status) {
    EXPECT_EQ(Status::NO_TRANSACTION_IN_PROGRESS, status);
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, NoTransactionRollback) {
  // Sequence of operations:
  //  - Rollback
  page_ptr_->Rollback([this](Status status) {
    EXPECT_EQ(Status::NO_TRANSACTION_IN_PROGRESS, status);
    message_loop_.PostQuitTask();
  });
  RunLoop();
}

TEST_F(PageImplTest, CreateReferenceFromSocket) {
  ASSERT_EQ(0u, fake_storage_->GetObjects().size());

  std::string value("a small value");
  Status status;
  ReferencePtr reference;
  page_ptr_->CreateReferenceFromSocket(
      value.size(), fsl::WriteStringToSocket(value),
      [this, &status, &reference](Status received_status,
                                  ReferencePtr received_reference) {
        status = received_status;
        reference = std::move(received_reference);
        message_loop_.PostQuitTask();
      });
  RunLoop();
  EXPECT_EQ(Status::OK, status);
  ASSERT_EQ(1u, fake_storage_->GetObjects().size());
  ASSERT_EQ(value, fake_storage_->GetObjects().begin()->second);
}

TEST_F(PageImplTest, CreateReferenceFromVmo) {
  ASSERT_EQ(0u, fake_storage_->GetObjects().size());

  std::string value("a small value");
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromString(value, &vmo));

  Status status;
  ReferencePtr reference;
  page_ptr_->CreateReferenceFromVmo(
      std::move(vmo).ToTransport(),
      [this, &status, &reference](Status received_status,
                                  ReferencePtr received_reference) {
        status = received_status;
        reference = std::move(received_reference);
        message_loop_.PostQuitTask();
      });
  RunLoop();
  EXPECT_EQ(Status::OK, status);
  ASSERT_EQ(1u, fake_storage_->GetObjects().size());
  ASSERT_EQ(value, fake_storage_->GetObjects().begin()->second);
}

TEST_F(PageImplTest, PutGetSnapshotGetEntries) {
  std::string eager_key("a_key");
  std::string eager_value("an eager value");
  std::string lazy_key("another_key");
  std::string lazy_value("a lazy value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };

  page_ptr_->Put(convert::ToArray(eager_key), convert::ToArray(eager_value),
                 callback_statusok);
  RunLoop();
  page_ptr_->PutWithPriority(convert::ToArray(lazy_key),
                             convert::ToArray(lazy_value), Priority::LAZY,
                             callback_statusok);
  RunLoop();
  PageSnapshotPtr snapshot = GetSnapshot();

  fidl::VectorPtr<Entry> actual_entries;
  auto callback_getentries = [this, &actual_entries](
                                 Status status, fidl::VectorPtr<Entry> entries,
                                 fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_entries = std::move(entries);
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, nullptr, callback_getentries);
  RunLoop();

  ASSERT_EQ(2u, actual_entries->size());
  EXPECT_EQ(eager_key, convert::ExtendedStringView(actual_entries->at(0).key));
  EXPECT_EQ(eager_value, ToString(actual_entries->at(0).value));
  EXPECT_EQ(Priority::EAGER, actual_entries->at(0).priority);

  EXPECT_EQ(lazy_key, convert::ExtendedStringView(actual_entries->at(1).key));
  EXPECT_EQ(lazy_value, ToString(actual_entries->at(1).value));
  EXPECT_EQ(Priority::LAZY, actual_entries->at(1).priority);
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesInline) {
  std::string eager_key("a_key");
  std::string eager_value("an eager value");
  std::string lazy_key("another_key");
  std::string lazy_value("a lazy value");

  Status status;

  page_ptr_->Put(convert::ToArray(eager_key), convert::ToArray(eager_value),
                 callback::Capture(MakeQuitTask(), &status));

  RunLoop();
  EXPECT_EQ(Status::OK, status);

  page_ptr_->PutWithPriority(convert::ToArray(lazy_key),
                             convert::ToArray(lazy_value), Priority::LAZY,
                             callback::Capture(MakeQuitTask(), &status));
  RunLoop();
  EXPECT_EQ(Status::OK, status);

  PageSnapshotPtr snapshot = GetSnapshot();

  fidl::VectorPtr<uint8_t> next_token;
  fidl::VectorPtr<InlinedEntry> actual_entries;
  snapshot->GetEntriesInline(
      nullptr, nullptr,
      callback::Capture(MakeQuitTask(), &status, &actual_entries, &next_token));
  RunLoop();
  EXPECT_EQ(Status::OK, status);
  EXPECT_TRUE(next_token.is_null());

  ASSERT_EQ(2u, actual_entries->size());
  EXPECT_EQ(eager_key, convert::ExtendedStringView(actual_entries->at(0).key));
  EXPECT_EQ(eager_value, convert::ToString(actual_entries->at(0).value));
  EXPECT_EQ(Priority::EAGER, actual_entries->at(0).priority);

  EXPECT_EQ(lazy_key, convert::ExtendedStringView(actual_entries->at(1).key));
  EXPECT_EQ(lazy_value, convert::ToString(actual_entries->at(1).value));
  EXPECT_EQ(Priority::LAZY, actual_entries->at(1).priority);
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesWithTokenForSize) {
  const size_t min_key_size = kMaxKeySize;
  // Put enough entries to ensure pagination of the result.
  // The number of entries in a Page is bounded by the maximum number of
  // handles, and the size of a fidl message (which cannot exceed
  // |kMaxInlineDataSize|), so we put one entry more than that.
  const size_t entry_count =
      std::min(fidl_serialization::kMaxMessageHandles,
               (fidl_serialization::kMaxInlineDataSize -
                fidl_serialization::kArrayHeaderSize) /
                   fidl_serialization::GetEntrySize(min_key_size)) +
      1;
  AddEntries(entry_count, min_key_size);
  PageSnapshotPtr snapshot = GetSnapshot();

  // Call GetEntries and find a partial result.
  fidl::VectorPtr<Entry> actual_entries;
  fidl::VectorPtr<uint8_t> actual_next_token;
  auto callback_getentries = [this, &actual_entries, &actual_next_token](
                                 Status status, fidl::VectorPtr<Entry> entries,
                                 fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::PARTIAL_RESULT, status);
    EXPECT_FALSE(next_token.is_null());
    actual_entries = std::move(entries);
    actual_next_token = std::move(next_token);
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, nullptr, callback_getentries);
  RunLoop();

  // Call GetEntries with the previous token and receive the remaining results.
  auto callback_getentries2 = [this, &actual_entries, &entry_count](
                                  Status status, fidl::VectorPtr<Entry> entries,
                                  fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    for (auto& entry : entries.take()) {
      actual_entries.push_back(std::move(entry));
    }
    EXPECT_EQ(static_cast<size_t>(entry_count), actual_entries->size());
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, std::move(actual_next_token),
                       callback_getentries2);
  RunLoop();

  // Check that the correct values of the keys are all present in the result and
  // in the correct order.
  for (int i = 0; i < static_cast<int>(actual_entries->size()); ++i) {
    ASSERT_EQ(GetKey(i, min_key_size),
              convert::ToString(actual_entries->at(i).key));
    ASSERT_EQ(GetValue(i, 0), ToString(actual_entries->at(i).value));
  }
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesInlineWithTokenForSize) {
  const size_t entry_count = 20;
  const size_t min_value_size =
      fidl_serialization::kMaxInlineDataSize * 3 / 2 / entry_count;
  AddEntries(entry_count, 0, min_value_size);
  PageSnapshotPtr snapshot = GetSnapshot();

  // Call GetEntries and find a partial result.
  Status status;
  fidl::VectorPtr<InlinedEntry> actual_entries;
  fidl::VectorPtr<uint8_t> actual_next_token;
  snapshot->GetEntriesInline(
      nullptr, nullptr,
      callback::Capture(MakeQuitTask(), &status, &actual_entries,
                        &actual_next_token));
  RunLoop();
  EXPECT_EQ(Status::PARTIAL_RESULT, status);
  EXPECT_FALSE(actual_next_token.is_null());

  // Call GetEntries with the previous token and receive the remaining results.
  fidl::VectorPtr<InlinedEntry> actual_entries2;
  fidl::VectorPtr<uint8_t> actual_next_token2;
  snapshot->GetEntriesInline(
      nullptr, std::move(actual_next_token),
      callback::Capture(MakeQuitTask(), &status, &actual_entries2,
                        &actual_next_token2));
  RunLoop();
  EXPECT_EQ(Status::OK, status);
  EXPECT_TRUE(actual_next_token2.is_null());
  for (auto& entry : actual_entries2.take()) {
    actual_entries.push_back(std::move(entry));
  }
  EXPECT_EQ(static_cast<size_t>(entry_count), actual_entries->size());

  // Check that the correct values of the keys are all present in the result and
  // in the correct order.
  for (int i = 0; i < static_cast<int>(actual_entries->size()); ++i) {
    ASSERT_EQ(GetKey(i, 0), convert::ToString(actual_entries->at(i).key));
    ASSERT_EQ(GetValue(i, min_value_size),
              convert::ToString(actual_entries->at(i).value));
  }
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesInlineWithTokenForEntryCount) {
  const size_t min_key_size = 8;
  const size_t min_value_size = 1;
  // Approximate size of the entry: takes into account size of the pointers for
  // key, object and entry itself; enum size for Priority and size of the header
  // for the InlinedEntry struct.
  const size_t min_entry_size =
      fidl_serialization::kPointerSize * 3 + fidl_serialization::kEnumSize +
      fidl_serialization::kStructHeaderSize +
      fidl_serialization::GetByteArraySize(min_key_size) +
      fidl_serialization::GetByteArraySize(min_value_size);
  // Put enough inlined entries to cause pagination based on size of the
  // message.
  const size_t entry_count =
      fidl_serialization::kMaxInlineDataSize * 3 / 2 / min_entry_size;
  AddEntries(entry_count, 0, min_value_size);
  PageSnapshotPtr snapshot = GetSnapshot();

  // Call GetEntries and find a partial result.
  Status status;
  fidl::VectorPtr<InlinedEntry> actual_entries;
  fidl::VectorPtr<uint8_t> actual_next_token;
  snapshot->GetEntriesInline(
      nullptr, nullptr,
      callback::Capture(MakeQuitTask(), &status, &actual_entries,
                        &actual_next_token));
  RunLoop();
  EXPECT_EQ(Status::PARTIAL_RESULT, status);
  EXPECT_FALSE(actual_next_token.is_null());

  // Call GetEntries with the previous token and receive the remaining results.
  fidl::VectorPtr<InlinedEntry> actual_entries2;
  fidl::VectorPtr<uint8_t> actual_next_token2;
  snapshot->GetEntriesInline(
      nullptr, std::move(actual_next_token),
      callback::Capture(MakeQuitTask(), &status, &actual_entries2,
                        &actual_next_token2));
  RunLoop();
  EXPECT_EQ(Status::OK, status);
  EXPECT_TRUE(actual_next_token2.is_null());
  for (auto& entry : actual_entries2.take()) {
    actual_entries.push_back(std::move(entry));
  }
  EXPECT_EQ(static_cast<size_t>(entry_count), actual_entries->size());

  // Check that the correct values of the keys are all present in the result and
  // in the correct order.
  for (int i = 0; i < static_cast<int>(actual_entries->size()); ++i) {
    ASSERT_EQ(GetKey(i, 0), convert::ToString(actual_entries->at(i).key));
    ASSERT_EQ(GetValue(i, min_value_size),
              convert::ToString(actual_entries->at(i).value));
  }
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesWithTokenForHandles) {
  const size_t entry_count = 100;
  AddEntries(entry_count);
  PageSnapshotPtr snapshot = GetSnapshot();

  // Call GetEntries and find a partial result.
  fidl::VectorPtr<Entry> actual_entries;
  fidl::VectorPtr<uint8_t> actual_next_token;
  auto callback_getentries = [this, &actual_entries, &actual_next_token](
                                 Status status, fidl::VectorPtr<Entry> entries,
                                 fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::PARTIAL_RESULT, status);
    EXPECT_FALSE(next_token.is_null());
    actual_entries = std::move(entries);
    actual_next_token = std::move(next_token);
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, nullptr, callback_getentries);
  RunLoop();

  // Call GetEntries with the previous token and receive the remaining results.
  auto callback_getentries2 = [this, &actual_entries](
                                  Status status, fidl::VectorPtr<Entry> entries,
                                  fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    for (auto& entry : entries.take()) {
      actual_entries.push_back(std::move(entry));
    }
    EXPECT_EQ(static_cast<size_t>(entry_count), actual_entries->size());
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, std::move(actual_next_token),
                       callback_getentries2);
  RunLoop();

  // Check that the correct values of the keys are all present in the result and
  // in the correct order.
  for (int i = 0; i < static_cast<int>(actual_entries->size()); ++i) {
    ASSERT_EQ(GetKey(i), convert::ToString(actual_entries->at(i).key));
    ASSERT_EQ(GetValue(i, 0), ToString(actual_entries->at(i).value));
  }
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesWithFetch) {
  std::string eager_key("a_key");
  std::string eager_value("an eager value");
  std::string lazy_key("another_key");
  std::string lazy_value("a lazy value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };

  page_ptr_->PutWithPriority(convert::ToArray(lazy_key),
                             convert::ToArray(lazy_value), Priority::LAZY,
                             callback_statusok);
  RunLoop();
  storage::ObjectIdentifier lazy_object_identifier =
      fake_storage_->GetObjects().begin()->first;

  page_ptr_->Put(convert::ToArray(eager_key), convert::ToArray(eager_value),
                 callback_statusok);
  RunLoop();

  fake_storage_->DeleteObjectFromLocal(lazy_object_identifier);

  PageSnapshotPtr snapshot = GetSnapshot();

  fidl::VectorPtr<Entry> actual_entries;
  auto callback_getentries = [this, &actual_entries](
                                 Status status, fidl::VectorPtr<Entry> entries,
                                 fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_entries = std::move(entries);
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, nullptr, callback_getentries);
  RunLoop();

  ASSERT_EQ(2u, actual_entries->size());
  EXPECT_EQ(eager_key, convert::ExtendedStringView(actual_entries->at(0).key));
  EXPECT_EQ(eager_value, ToString(actual_entries->at(0).value));
  EXPECT_EQ(Priority::EAGER, actual_entries->at(0).priority);

  EXPECT_EQ(lazy_key, convert::ExtendedStringView(actual_entries->at(1).key));
  EXPECT_FALSE(actual_entries->at(1).value);
  EXPECT_EQ(Priority::LAZY, actual_entries->at(1).priority);
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesWithPrefix) {
  std::string eager_key("001-a_key");
  std::string eager_value("an eager value");
  std::string lazy_key("002-another_key");
  std::string lazy_value("a lazy value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };

  page_ptr_->Put(convert::ToArray(eager_key), convert::ToArray(eager_value),
                 callback_statusok);
  RunLoop();
  page_ptr_->PutWithPriority(convert::ToArray(lazy_key),
                             convert::ToArray(lazy_value), Priority::LAZY,
                             callback_statusok);
  RunLoop();

  PageSnapshotPtr snapshot = GetSnapshot(convert::ToArray("001"));
  fidl::VectorPtr<Entry> actual_entries;
  auto callback_getentries = [this, &actual_entries](
                                 Status status, fidl::VectorPtr<Entry> entries,
                                 fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_entries = std::move(entries);
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(nullptr, nullptr, callback_getentries);
  RunLoop();

  ASSERT_EQ(1u, actual_entries->size());
  EXPECT_EQ(eager_key, convert::ExtendedStringView(actual_entries->at(0).key));

  snapshot = GetSnapshot(convert::ToArray("00"));
  snapshot->GetEntries(nullptr, nullptr, callback_getentries);
  RunLoop();

  ASSERT_EQ(2u, actual_entries->size());
  EXPECT_EQ(eager_key, convert::ExtendedStringView(actual_entries->at(0).key));
  EXPECT_EQ(lazy_key, convert::ExtendedStringView(actual_entries->at(1).key));
}

TEST_F(PageImplTest, PutGetSnapshotGetEntriesWithStart) {
  std::string eager_key("001-a_key");
  std::string eager_value("an eager value");
  std::string lazy_key("002-another_key");
  std::string lazy_value("a lazy value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };

  page_ptr_->Put(convert::ToArray(eager_key), convert::ToArray(eager_value),
                 callback_statusok);
  RunLoop();
  page_ptr_->PutWithPriority(convert::ToArray(lazy_key),
                             convert::ToArray(lazy_value), Priority::LAZY,
                             callback_statusok);
  RunLoop();

  PageSnapshotPtr snapshot = GetSnapshot();
  fidl::VectorPtr<Entry> actual_entries;
  auto callback_getentries = [this, &actual_entries](
                                 Status status, fidl::VectorPtr<Entry> entries,
                                 fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_entries = std::move(entries);
    message_loop_.PostQuitTask();
  };
  snapshot->GetEntries(convert::ToArray("002"), nullptr, callback_getentries);
  RunLoop();

  ASSERT_EQ(1u, actual_entries->size());
  EXPECT_EQ(lazy_key, convert::ExtendedStringView(actual_entries->at(0).key));

  snapshot->GetEntries(convert::ToArray("001"), nullptr, callback_getentries);
  RunLoop();

  ASSERT_EQ(2u, actual_entries->size());
  EXPECT_EQ(eager_key, convert::ExtendedStringView(actual_entries->at(0).key));
  EXPECT_EQ(lazy_key, convert::ExtendedStringView(actual_entries->at(1).key));
}

TEST_F(PageImplTest, PutGetSnapshotGetKeys) {
  std::string key1("some_key");
  std::string value1("a small value");
  std::string key2("some_key2");
  std::string value2("another value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->StartTransaction(callback_statusok);
  RunLoop();
  page_ptr_->Put(convert::ToArray(key1), convert::ToArray(value1),
                 callback_statusok);
  RunLoop();
  page_ptr_->Put(convert::ToArray(key2), convert::ToArray(value2),
                 callback_statusok);
  RunLoop();
  page_ptr_->Commit(callback_statusok);
  RunLoop();
  PageSnapshotPtr snapshot = GetSnapshot();

  fidl::VectorPtr<fidl::VectorPtr<uint8_t>> actual_keys;
  auto callback_getkeys = [this, &actual_keys](
                              Status status,
                              fidl::VectorPtr<fidl::VectorPtr<uint8_t>> keys,
                              fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_keys = std::move(keys);
    message_loop_.PostQuitTask();
  };
  snapshot->GetKeys(nullptr, nullptr, callback_getkeys);
  RunLoop();

  EXPECT_EQ(2u, actual_keys->size());
  EXPECT_EQ(key1, convert::ExtendedStringView(actual_keys->at(0)));
  EXPECT_EQ(key2, convert::ExtendedStringView(actual_keys->at(1)));
}

TEST_F(PageImplTest, PutGetSnapshotGetKeysWithToken) {
  const size_t min_key_size = kMaxKeySize;
  const size_t key_count =
      fidl_serialization::kMaxInlineDataSize /
          fidl_serialization::GetByteArraySize(min_key_size) +
      1;
  AddEntries(key_count, min_key_size);
  PageSnapshotPtr snapshot = GetSnapshot();

  // Call GetKeys and find a partial result.
  fidl::VectorPtr<fidl::VectorPtr<uint8_t>> actual_keys;
  fidl::VectorPtr<uint8_t> actual_next_token;
  auto callback_getkeys = [this, &actual_keys, &actual_next_token](
                              Status status,
                              fidl::VectorPtr<fidl::VectorPtr<uint8_t>> keys,
                              fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::PARTIAL_RESULT, status);
    EXPECT_FALSE(next_token.is_null());
    actual_keys = std::move(keys);
    actual_next_token = std::move(next_token);
    message_loop_.PostQuitTask();
  };
  snapshot->GetKeys(nullptr, nullptr, callback_getkeys);
  RunLoop();

  // Call GetKeys with the previous token and receive the remaining results.
  auto callback_getkeys2 = [this, &actual_keys, &key_count](
                               Status status,
                               fidl::VectorPtr<fidl::VectorPtr<uint8_t>> keys,
                               fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    for (auto& key : keys.take()) {
      actual_keys.push_back(std::move(key));
    }
    EXPECT_EQ(static_cast<size_t>(key_count), actual_keys->size());
    message_loop_.PostQuitTask();
  };
  snapshot->GetKeys(nullptr, std::move(actual_next_token), callback_getkeys2);
  RunLoop();

  // Check that the correct values of the keys are all present in the result and
  // in the correct order.
  for (size_t i = 0; i < actual_keys->size(); ++i) {
    ASSERT_EQ(GetKey(i, min_key_size), convert::ToString(actual_keys->at(i)));
  }
}

TEST_F(PageImplTest, PutGetSnapshotGetKeysWithPrefix) {
  std::string key1("001-some_key");
  std::string value1("a small value");
  std::string key2("002-some_key2");
  std::string value2("another value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->StartTransaction(callback_statusok);
  RunLoop();
  page_ptr_->Put(convert::ToArray(key1), convert::ToArray(value1),
                 callback_statusok);
  RunLoop();
  page_ptr_->Put(convert::ToArray(key2), convert::ToArray(value2),
                 callback_statusok);
  RunLoop();
  page_ptr_->Commit(callback_statusok);
  RunLoop();

  PageSnapshotPtr snapshot = GetSnapshot(convert::ToArray("001"));

  fidl::VectorPtr<fidl::VectorPtr<uint8_t>> actual_keys;
  auto callback_getkeys = [this, &actual_keys](
                              Status status,
                              fidl::VectorPtr<fidl::VectorPtr<uint8_t>> keys,
                              fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_keys = std::move(keys);
    message_loop_.PostQuitTask();
  };
  snapshot->GetKeys(nullptr, nullptr, callback_getkeys);
  RunLoop();

  EXPECT_EQ(1u, actual_keys->size());
  EXPECT_EQ(key1, convert::ExtendedStringView(actual_keys->at(0)));

  snapshot = GetSnapshot(convert::ToArray("00"));
  snapshot->GetKeys(nullptr, nullptr, callback_getkeys);
  RunLoop();

  EXPECT_EQ(2u, actual_keys->size());
  EXPECT_EQ(key1, convert::ExtendedStringView(actual_keys->at(0)));
  EXPECT_EQ(key2, convert::ExtendedStringView(actual_keys->at(1)));
}

TEST_F(PageImplTest, PutGetSnapshotGetKeysWithStart) {
  std::string key1("001-some_key");
  std::string value1("a small value");
  std::string key2("002-some_key2");
  std::string value2("another value");

  auto callback_statusok = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->StartTransaction(callback_statusok);
  RunLoop();
  page_ptr_->Put(convert::ToArray(key1), convert::ToArray(value1),
                 callback_statusok);
  RunLoop();
  page_ptr_->Put(convert::ToArray(key2), convert::ToArray(value2),
                 callback_statusok);
  RunLoop();
  page_ptr_->Commit(callback_statusok);
  RunLoop();

  PageSnapshotPtr snapshot = GetSnapshot();

  fidl::VectorPtr<fidl::VectorPtr<uint8_t>> actual_keys;
  auto callback_getkeys = [this, &actual_keys](
                              Status status,
                              fidl::VectorPtr<fidl::VectorPtr<uint8_t>> keys,
                              fidl::VectorPtr<uint8_t> next_token) {
    EXPECT_EQ(Status::OK, status);
    EXPECT_TRUE(next_token.is_null());
    actual_keys = std::move(keys);
    message_loop_.PostQuitTask();
  };
  snapshot->GetKeys(convert::ToArray("002"), nullptr, callback_getkeys);
  RunLoop();

  EXPECT_EQ(1u, actual_keys->size());
  EXPECT_EQ(key2, convert::ExtendedStringView(actual_keys->at(0)));

  snapshot = GetSnapshot();
  snapshot->GetKeys(convert::ToArray("001"), nullptr, callback_getkeys);
  RunLoop();

  EXPECT_EQ(2u, actual_keys->size());
  EXPECT_EQ(key1, convert::ExtendedStringView(actual_keys->at(0)));
  EXPECT_EQ(key2, convert::ExtendedStringView(actual_keys->at(1)));
}

TEST_F(PageImplTest, SnapshotGetSmall) {
  std::string key("some_key");
  std::string value("a small value");

  auto callback_put = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value), callback_put);
  RunLoop();
  PageSnapshotPtr snapshot = GetSnapshot();

  mem::BufferPtr actual_value;
  auto callback_get = [this, &actual_value](Status status,
                                            mem::BufferPtr value) {
    EXPECT_EQ(Status::OK, status);
    actual_value = std::move(value);
    message_loop_.PostQuitTask();
  };
  snapshot->Get(convert::ToArray(key), callback_get);
  RunLoop();

  EXPECT_EQ(value, ToString(actual_value));

  fidl::VectorPtr<uint8_t> actual_inlined_value;
  auto callback_get_inline = [this, &actual_inlined_value](
                                 Status status,
                                 fidl::VectorPtr<uint8_t> value) {
    EXPECT_EQ(Status::OK, status);
    actual_inlined_value = std::move(value);
    message_loop_.PostQuitTask();
  };

  snapshot->GetInline(convert::ToArray(key), callback_get_inline);
  RunLoop();
  EXPECT_EQ(value, convert::ToString(actual_inlined_value));
}

TEST_F(PageImplTest, SnapshotGetLarge) {
  std::string value_string(fidl_serialization::kMaxInlineDataSize + 1, 'a');
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromString(value_string, &vmo));

  Status status;
  ReferencePtr reference;
  page_ptr_->CreateReferenceFromVmo(
      std::move(vmo).ToTransport(),
      callback::Capture(MakeQuitTask(), &status, &reference));
  RunLoop();

  ASSERT_EQ(Status::OK, status);

  std::string key("some_key");
  page_ptr_->PutReference(convert::ToArray(key), std::move(*reference),
                          Priority::EAGER,
                          callback::Capture(MakeQuitTask(), &status));
  RunLoop();
  EXPECT_EQ(Status::OK, status);

  PageSnapshotPtr snapshot = GetSnapshot();

  mem::BufferPtr actual_value;
  snapshot->Get(convert::ExtendedStringView(key).ToArray(),
                callback::Capture(MakeQuitTask(), &status, &actual_value));
  RunLoop();
  EXPECT_EQ(Status::OK, status);

  EXPECT_EQ(value_string, ToString(actual_value));

  fidl::VectorPtr<uint8_t> array_value;
  snapshot->GetInline(convert::ToArray(key),
                      callback::Capture(MakeQuitTask(), &status, &array_value));
  RunLoop();
  EXPECT_EQ(Status::VALUE_TOO_LARGE, status);
}

TEST_F(PageImplTest, SnapshotGetNeedsFetch) {
  std::string key("some_key");
  std::string value("a small value");

  Status status;
  auto postquit_callback = MakeQuitTask();
  page_ptr_->PutWithPriority(convert::ToArray(key), convert::ToArray(value),
                             Priority::LAZY,
                             ::callback::Capture(postquit_callback, &status));
  RunLoop();
  EXPECT_EQ(Status::OK, status);

  storage::ObjectIdentifier lazy_object_identifier =
      fake_storage_->GetObjects().begin()->first;
  fake_storage_->DeleteObjectFromLocal(lazy_object_identifier);

  PageSnapshotPtr snapshot = GetSnapshot();

  mem::BufferPtr actual_value;
  snapshot->Get(convert::ToArray(key),
                ::callback::Capture(postquit_callback, &status, &actual_value));
  RunLoop();

  EXPECT_EQ(Status::NEEDS_FETCH, status);
  EXPECT_FALSE(actual_value);

  fidl::VectorPtr<uint8_t> actual_inlined_value;
  snapshot->GetInline(
      convert::ToArray(key),
      ::callback::Capture(postquit_callback, &status, &actual_inlined_value));
  RunLoop();

  EXPECT_EQ(Status::NEEDS_FETCH, status);
  EXPECT_FALSE(actual_inlined_value);
}

TEST_F(PageImplTest, SnapshotFetchPartial) {
  std::string key("some_key");
  std::string value("a small value");

  auto callback_put = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value), callback_put);
  RunLoop();
  PageSnapshotPtr snapshot = GetSnapshot();

  Status status;
  mem::BufferPtr buffer;
  snapshot->FetchPartial(
      convert::ToArray(key), 2, 5,
      [this, &status, &buffer](Status received_status,
                               mem::BufferPtr received_buffer) {
        status = received_status;
        buffer = std::move(received_buffer);
        message_loop_.PostQuitTask();
      });
  RunLoop();
  EXPECT_EQ(Status::OK, status);
  std::string content;
  EXPECT_TRUE(fsl::StringFromVmo(*buffer, &content));
  EXPECT_EQ("small", content);
}

TEST_F(PageImplTest, ParallelPut) {
  Status status;
  PagePtr page_ptr2;
  manager_->BindPage(page_ptr2.NewRequest(),
                     callback::Capture(MakeQuitTask(), &status));
  RunLoop();
  ASSERT_EQ(Status::OK, status);

  std::string key("some_key");
  std::string value1("a small value");
  std::string value2("another value");

  PageSnapshotPtr snapshot1;
  PageSnapshotPtr snapshot2;

  auto callback_simple = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->StartTransaction(callback_simple);
  RunLoop();

  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value1),
                 callback_simple);
  RunLoop();

  page_ptr2->StartTransaction(callback_simple);
  RunLoop();

  page_ptr2->Put(convert::ToArray(key), convert::ToArray(value2),
                 callback_simple);
  RunLoop();

  page_ptr_->Commit(callback_simple);
  RunLoop();
  page_ptr2->Commit(callback_simple);
  RunLoop();

  auto callback_getsnapshot = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };
  page_ptr_->GetSnapshot(snapshot1.NewRequest(), nullptr, nullptr,
                         callback_getsnapshot);
  RunLoop();
  page_ptr2->GetSnapshot(snapshot2.NewRequest(), nullptr, nullptr,
                         callback_getsnapshot);
  RunLoop();

  std::string actual_value1;
  auto callback_getvalue1 = [this, &actual_value1](
                                Status status, mem::BufferPtr returned_value) {
    EXPECT_EQ(Status::OK, status);
    actual_value1 = ToString(returned_value);
    message_loop_.PostQuitTask();
  };
  snapshot1->Get(convert::ToArray(key), callback_getvalue1);
  RunLoop();

  std::string actual_value2;
  auto callback_getvalue2 = [this, &actual_value2](
                                Status status, mem::BufferPtr returned_value) {
    EXPECT_EQ(Status::OK, status);
    actual_value2 = ToString(returned_value);
    message_loop_.PostQuitTask();
  };
  snapshot2->Get(convert::ToArray(key), callback_getvalue2);
  RunLoop();

  // The two snapshots should have different contents.
  EXPECT_EQ(value1, actual_value1);
  EXPECT_EQ(value2, actual_value2);
}

TEST_F(PageImplTest, SerializedOperations) {
  fake_storage_->set_autocommit(false);

  std::string key("some_key");
  std::string value1("a value");
  std::string value2("a second value");
  std::string value3("a third value");

  auto callback_simple = [this](Status status) {
    EXPECT_EQ(Status::OK, status);
    message_loop_.PostQuitTask();
  };

  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value1),
                 callback_simple);
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value2),
                 callback_simple);
  page_ptr_->Delete(convert::ToArray(key), callback_simple);
  page_ptr_->StartTransaction(callback_simple);
  page_ptr_->Put(convert::ToArray(key), convert::ToArray(value3),
                 callback_simple);
  page_ptr_->Commit(callback_simple);

  // 3 first operations need to be serialized and blocked on commits.
  for (size_t i = 0; i < 3; ++i) {
    // Callbacks are blocked until operation commits.
    EXPECT_TRUE(RunLoopWithTimeout(fxl::TimeDelta::FromMilliseconds(20)));

    // The commit queue contains the new commit.
    ASSERT_EQ(i + 1, fake_storage_->GetJournals().size());
    CommitFirstPendingJournal(fake_storage_->GetJournals());

    // The operation can now succeed.
    RunLoop();
  }

  // Neither StartTransaction, nor Put in a transaction should be blocked.
  for (size_t i = 0; i < 2; ++i) {
    RunLoop();
  }

  // But committing the transaction should still be blocked.
  EXPECT_TRUE(RunLoopWithTimeout(fxl::TimeDelta::FromMilliseconds(20)));

  // Unblocking the transaction commit.
  CommitFirstPendingJournal(fake_storage_->GetJournals());
  // The operation can now succeed.
  RunLoop();
}

TEST_F(PageImplTest, WaitForConflictResolutionNoConflicts) {
  bool callback_called = false;

  auto conflicts_resolved_callback = [this, &callback_called]() {
    EXPECT_TRUE(resolver_->IsEmpty());
    callback_called = true;
    message_loop_.PostQuitTask();
  };

  ConflictResolutionWaitStatus status;
  page_ptr_->WaitForConflictResolution(
      callback::Capture(conflicts_resolved_callback, &status));
  RunLoop();
  ASSERT_TRUE(callback_called);
  EXPECT_EQ(ConflictResolutionWaitStatus::NO_CONFLICTS, status);

  // Special case: no changes from the previous call; event OnEmpty is not
  // triggered, but WaitForConflictResolution should return right away, as there
  // are no pending merges.
  callback_called = false;
  page_ptr_->WaitForConflictResolution(
      callback::Capture(conflicts_resolved_callback, &status));
  RunLoop();
  ASSERT_TRUE(callback_called);
  EXPECT_EQ(ConflictResolutionWaitStatus::NO_CONFLICTS, status);
}

}  // namespace
}  // namespace ledger
