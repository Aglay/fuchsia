// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/fxl/time/time_delta.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "peridot/bin/ledger/app/constants.h"
#include "peridot/bin/ledger/app/fidl/serialization_size.h"
#include "peridot/bin/ledger/tests/integration/integration_test.h"
#include "peridot/bin/ledger/tests/integration/test_utils.h"
#include "peridot/lib/callback/capture.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace integration {
namespace {

class PageWatcherIntegrationTest : public IntegrationTest {
 public:
  PageWatcherIntegrationTest() {}
  ~PageWatcherIntegrationTest() override {}

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageWatcherIntegrationTest);
};

class Watcher : public ledger::PageWatcher {
 public:
  Watcher(f1dl::InterfaceRequest<PageWatcher> request,
          fxl::Closure change_callback)
      : binding_(this, std::move(request)),
        change_callback_(std::move(change_callback)) {}

  uint changes_seen = 0;
  ledger::ResultState last_result_state_;
  ledger::PageSnapshotPtr last_snapshot_;
  ledger::PageChangePtr last_page_change_;

 private:
  // PageWatcher:
  void OnChange(ledger::PageChangePtr page_change,
                ledger::ResultState result_state,
                const OnChangeCallback& callback) override {
    FXL_DCHECK(page_change);
    changes_seen++;
    last_result_state_ = result_state;
    last_page_change_ = std::move(page_change);
    last_snapshot_.Unbind();
    callback(last_snapshot_.NewRequest());
    change_callback_();
  }

  f1dl::Binding<PageWatcher> binding_;
  fxl::Closure change_callback_;
};

TEST_F(PageWatcherIntegrationTest, PageWatcherSimple) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher.last_result_state_);
  ledger::PageChangePtr change = std::move(watcher.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", ToString(change->changes[0]->value));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherDisconnectClient) {
  auto instance = NewLedgerAppInstance();
  ledger::Status status;
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  auto watcher = std::make_unique<Watcher>(watcher_ptr.NewRequest(), [] {
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  // Make a change on the page and verify that it was received.
  page->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1u, watcher->changes_seen);

  // Make another change and disconnect the watcher immediately.
  page->Put(convert::ToArray("name"), convert::ToArray("Bob"),
            callback::Capture(MakeQuitTask(), &status));
  watcher.reset();
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(ledger::Status::OK, status);
}

TEST_F(PageWatcherIntegrationTest, PageWatcherDisconnectPage) {
  auto instance = NewLedgerAppInstance();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  {
    ledger::PagePtr page = instance->GetTestPage();
    ledger::PageSnapshotPtr snapshot;
    page->GetSnapshot(
        snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
        [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
    EXPECT_TRUE(page.WaitForResponse());

    // Queue many put operations on the page.
    for (int i = 0; i < 1000; i++) {
      page->Put(
          convert::ToArray("name"), convert::ToArray(std::to_string(i)),
          [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
    }
  }
  // Page is out of scope now, but watcher is not. Verify that we don't crash
  // and a change notification is still delivered.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1u, watcher.changes_seen);
}

TEST_F(PageWatcherIntegrationTest, PageWatcherDelete) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  page->Put(
      convert::ToArray("foo"), convert::ToArray("bar"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->Delete(convert::ToArray("foo"), [](ledger::Status status) {
    EXPECT_EQ(ledger::Status::OK, status);
  });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher.last_result_state_);
  ledger::PageChangePtr change = std::move(watcher.last_page_change_);
  EXPECT_EQ(0u, change->changes.size());
  ASSERT_EQ(1u, change->deleted_keys.size());
  EXPECT_EQ("foo", convert::ToString(change->deleted_keys[0]));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherBigChangeSize) {
  auto instance = NewLedgerAppInstance();
  // Put enough entries to ensure we will need more than one query to retrieve
  // them. The number of entries that can be retrieved in one query is bound by
  // |kMaxMessageHandles| and by size of the fidl message (determined by
  // |kMaxInlineDataSize|), so we insert one entry more than that.
  const size_t key_size = ledger::kMaxKeySize;
  const size_t entry_size = ledger::fidl_serialization::GetEntrySize(key_size);
  const size_t entry_count =
      std::min(ledger::fidl_serialization::kMaxMessageHandles,
               ledger::fidl_serialization::kMaxInlineDataSize / entry_size) +
      1;
  const auto key_generator = [](size_t i) {
    std::string prefix = fxl::StringPrintf("key%03" PRIuMAX, i);
    std::string filler(key_size - prefix.size(), 'k');
    return prefix + filler;
  };
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->StartTransaction(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  for (size_t i = 0; i < entry_count; ++i) {
    page->Put(
        convert::ToArray(key_generator(i)), convert::ToArray("value"),
        [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
    EXPECT_TRUE(page.WaitForResponse());
  }

  EXPECT_TRUE(RunLoopWithTimeout(fxl::TimeDelta::FromMilliseconds(100)));
  EXPECT_EQ(0u, watcher.changes_seen);

  page->Commit(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  // Get the first OnChagne call.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(watcher.last_result_state_, ledger::ResultState::PARTIAL_STARTED);
  ledger::PageChangePtr change = std::move(watcher.last_page_change_);
  size_t initial_size = change->changes.size();
  for (size_t i = 0; i < initial_size; ++i) {
    EXPECT_EQ(key_generator(i), convert::ToString(change->changes[i]->key));
    EXPECT_EQ("value", ToString(change->changes[i]->value));
    EXPECT_EQ(ledger::Priority::EAGER, change->changes[i]->priority);
  }

  // Get the second OnChagne call.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(2u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::PARTIAL_COMPLETED, watcher.last_result_state_);
  change = std::move(watcher.last_page_change_);

  ASSERT_EQ(entry_count, initial_size + change->changes.size());
  for (size_t i = 0; i < change->changes.size(); ++i) {
    EXPECT_EQ(key_generator(i + initial_size),
              convert::ToString(change->changes[i]->key));
    EXPECT_EQ("value", ToString(change->changes[i]->value));
    EXPECT_EQ(ledger::Priority::EAGER, change->changes[i]->priority);
  }
}

TEST_F(PageWatcherIntegrationTest, PageWatcherBigChangeHandles) {
  auto instance = NewLedgerAppInstance();
  size_t entry_count = 70;
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->StartTransaction(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  for (size_t i = 0; i < entry_count; ++i) {
    page->Put(convert::ToArray(fxl::StringPrintf("key%02" PRIuMAX, i)),
              convert::ToArray("value"), [](ledger::Status status) {
                EXPECT_EQ(ledger::Status::OK, status);
              });
    EXPECT_TRUE(page.WaitForResponse());
  }

  EXPECT_TRUE(RunLoopWithTimeout(fxl::TimeDelta::FromMilliseconds(100)));
  EXPECT_EQ(0u, watcher.changes_seen);

  page->Commit(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  // Get the first OnChagne call.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(watcher.last_result_state_, ledger::ResultState::PARTIAL_STARTED);
  ledger::PageChangePtr change = std::move(watcher.last_page_change_);
  size_t initial_size = change->changes.size();
  for (size_t i = 0; i < initial_size; ++i) {
    EXPECT_EQ(fxl::StringPrintf("key%02" PRIuMAX, i),
              convert::ToString(change->changes[i]->key));
    EXPECT_EQ("value", ToString(change->changes[i]->value));
    EXPECT_EQ(ledger::Priority::EAGER, change->changes[i]->priority);
  }

  // Get the second OnChagne call.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(2u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::PARTIAL_COMPLETED, watcher.last_result_state_);
  change = std::move(watcher.last_page_change_);

  ASSERT_EQ(entry_count, initial_size + change->changes.size());
  for (size_t i = 0; i < change->changes.size(); ++i) {
    EXPECT_EQ(fxl::StringPrintf("key%02" PRIuMAX, i + initial_size),
              convert::ToString(change->changes[i]->key));
    EXPECT_EQ("value", ToString(change->changes[i]->value));
    EXPECT_EQ(ledger::Priority::EAGER, change->changes[i]->priority);
  }
}

TEST_F(PageWatcherIntegrationTest, PageWatcherSnapshot) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher.last_result_state_);
  f1dl::Array<ledger::EntryPtr> entries =
      SnapshotGetEntries(&(watcher.last_snapshot_), convert::ToArray(""));
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ("name", convert::ToString(entries[0]->key));
  EXPECT_EQ("Alice", ToString(entries[0]->value));
  EXPECT_EQ(ledger::Priority::EAGER, entries[0]->priority);
}

TEST_F(PageWatcherIntegrationTest, PageWatcherTransaction) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->StartTransaction(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  page->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  EXPECT_TRUE(RunLoopWithTimeout());
  EXPECT_EQ(0u, watcher.changes_seen);

  page->Commit(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher.last_result_state_);
  ledger::PageChangePtr change = std::move(watcher.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", ToString(change->changes[0]->value));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherParallel) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page1 = instance->GetTestPage();
  f1dl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](f1dl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForResponse());

  ledger::PagePtr page2 = instance->GetPage(test_page_id, ledger::Status::OK);

  ledger::PageWatcherPtr watcher1_ptr;
  Watcher watcher1(watcher1_ptr.NewRequest(),
                   [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });
  ledger::PageSnapshotPtr snapshot1;
  page1->GetSnapshot(
      snapshot1.NewRequest(), nullptr, std::move(watcher1_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page1.WaitForResponse());

  ledger::PageWatcherPtr watcher2_ptr;
  Watcher watcher2(watcher2_ptr.NewRequest(),
                   [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });
  ledger::PageSnapshotPtr snapshot2;
  page2->GetSnapshot(
      snapshot2.NewRequest(), nullptr, std::move(watcher2_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page2.WaitForResponse());

  page1->StartTransaction(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page1.WaitForResponse());
  page1->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page1.WaitForResponse());

  page2->StartTransaction(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page2.WaitForResponse());
  page2->Put(
      convert::ToArray("name"), convert::ToArray("Bob"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page2.WaitForResponse());

  // Verify that each change is seen by the right watcher.
  page1->Commit(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page1.WaitForResponse());
  fsl::MessageLoop::GetCurrent()->Run();
  EXPECT_EQ(1u, watcher1.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher1.last_result_state_);
  ledger::PageChangePtr change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", ToString(change->changes[0]->value));

  page2->Commit(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page2.WaitForResponse());
  fsl::MessageLoop::GetCurrent()->Run();

  EXPECT_EQ(1u, watcher2.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher2.last_result_state_);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", ToString(change->changes[0]->value));

  fsl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); },
      fxl::TimeDelta::FromSeconds(1));
  fsl::MessageLoop::GetCurrent()->Run();
  // A merge happens now. Only the first watcher should see a change.
  EXPECT_EQ(2u, watcher1.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher2.last_result_state_);
  EXPECT_EQ(1u, watcher2.changes_seen);

  change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", ToString(change->changes[0]->value));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherEmptyTransaction) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->StartTransaction(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->Commit(
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_TRUE(RunLoopWithTimeout());
  EXPECT_EQ(0u, watcher.changes_seen);
}

TEST_F(PageWatcherIntegrationTest, PageWatcher1Change2Pages) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page1 = instance->GetTestPage();
  f1dl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](f1dl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForResponse());

  ledger::PagePtr page2 = instance->GetPage(test_page_id, ledger::Status::OK);

  ledger::PageWatcherPtr watcher1_ptr;
  Watcher watcher1(watcher1_ptr.NewRequest(),
                   [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });
  ledger::PageSnapshotPtr snapshot1;
  page1->GetSnapshot(
      snapshot1.NewRequest(), nullptr, std::move(watcher1_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page1.WaitForResponse());

  ledger::PageWatcherPtr watcher2_ptr;
  Watcher watcher2(watcher2_ptr.NewRequest(),
                   [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });
  ledger::PageSnapshotPtr snapshot2;
  page2->GetSnapshot(
      snapshot2.NewRequest(), nullptr, std::move(watcher2_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page2.WaitForResponse());

  page1->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page1.WaitForResponse());

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(1u, watcher1.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher1.last_result_state_);
  ledger::PageChangePtr change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", ToString(change->changes[0]->value));

  ASSERT_EQ(1u, watcher2.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher2.last_result_state_);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", ToString(change->changes[0]->value));
}

class WaitingWatcher : public ledger::PageWatcher {
 public:
  WaitingWatcher(f1dl::InterfaceRequest<ledger::PageWatcher> request,
                 fxl::Closure change_callback)
      : binding_(this, std::move(request)),
        change_callback_(std::move(change_callback)) {}

  struct Change {
    ledger::PageChangePtr change;
    OnChangeCallback callback;

    Change(ledger::PageChangePtr change, OnChangeCallback callback)
        : change(std::move(change)), callback(std::move(callback)) {}
  };

  std::vector<Change> changes;

 private:
  // PageWatcher:
  void OnChange(ledger::PageChangePtr page_change,
                ledger::ResultState result_state,
                const OnChangeCallback& callback) override {
    FXL_DCHECK(page_change);
    FXL_DCHECK(result_state == ledger::ResultState::COMPLETED)
        << "Handling OnChange pagination not implemented yet";
    changes.emplace_back(std::move(page_change), callback);
    change_callback_();
  }

  f1dl::Binding<ledger::PageWatcher> binding_;
  fxl::Closure change_callback_;
};

TEST_F(PageWatcherIntegrationTest, PageWatcherConcurrentTransaction) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  WaitingWatcher watcher(watcher_ptr.NewRequest(), []() {
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  });

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(
      snapshot.NewRequest(), nullptr, std::move(watcher_ptr),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  page->Put(
      convert::ToArray("name"), convert::ToArray("Alice"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes.size());

  page->Put(
      convert::ToArray("foo"), convert::ToArray("bar"),
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page.WaitForResponse());

  bool start_transaction_callback_called = false;
  ledger::Status start_transaction_status;
  page->StartTransaction([&start_transaction_callback_called,
                          &start_transaction_status](ledger::Status status) {
    start_transaction_callback_called = true;
    start_transaction_status = status;
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  });

  EXPECT_TRUE(RunLoopWithTimeout());

  // We haven't sent the callback of the first change, so nothing should have
  // happened.
  EXPECT_EQ(1u, watcher.changes.size());
  EXPECT_FALSE(start_transaction_callback_called);

  watcher.changes[0].callback(nullptr);

  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(2u, watcher.changes.size());
  EXPECT_FALSE(start_transaction_callback_called);

  EXPECT_TRUE(RunLoopWithTimeout());

  // We haven't sent the callback of the first change, so nothing should have
  // happened.
  EXPECT_EQ(2u, watcher.changes.size());
  EXPECT_FALSE(start_transaction_callback_called);

  watcher.changes[1].callback(nullptr);

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(start_transaction_callback_called);
  EXPECT_EQ(ledger::Status::OK, start_transaction_status);
}

TEST_F(PageWatcherIntegrationTest, PageWatcherPrefix) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  auto callback_statusok = [](ledger::Status status) {
    EXPECT_EQ(ledger::Status::OK, status);
  };
  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(snapshot.NewRequest(), convert::ToArray("01"),
                    std::move(watcher_ptr), callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());

  page->StartTransaction(callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());
  page->Put(convert::ToArray("00-key"), convert::ToArray("value-00"),
            callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());
  page->Put(convert::ToArray("01-key"), convert::ToArray("value-01"),
            callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());
  page->Put(convert::ToArray("02-key"), convert::ToArray("value-02"),
            callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());
  page->Commit(callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  EXPECT_EQ(ledger::ResultState::COMPLETED, watcher.last_result_state_);
  ledger::PageChangePtr change = std::move(watcher.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("01-key", convert::ToString(change->changes[0]->key));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherPrefixNoChange) {
  auto instance = NewLedgerAppInstance();
  ledger::PagePtr page = instance->GetTestPage();
  ledger::PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); });

  auto callback_statusok = [](ledger::Status status) {
    EXPECT_EQ(ledger::Status::OK, status);
  };
  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(snapshot.NewRequest(), convert::ToArray("01"),
                    std::move(watcher_ptr), callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());

  page->Put(convert::ToArray("00-key"), convert::ToArray("value-00"),
            callback_statusok);
  EXPECT_TRUE(page.WaitForResponse());

  page->StartTransaction([](ledger::Status status) {
    EXPECT_EQ(ledger::Status::OK, status);
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  });
  EXPECT_FALSE(RunLoopWithTimeout());

  // Starting a transaction drains all watcher notifications, so if we were to
  // be called, we would know at this point.
  EXPECT_EQ(0u, watcher.changes_seen);
}

}  // namespace
}  // namespace integration
}  // namespace test
