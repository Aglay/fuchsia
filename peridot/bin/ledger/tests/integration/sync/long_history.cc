// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/callback/capture.h>
#include <lib/callback/waiter.h>
#include <lib/fidl/cpp/optional.h>

#include "peridot/bin/ledger/tests/integration/integration_test.h"
#include "peridot/bin/ledger/tests/integration/sync/test_sync_state_watcher.h"
#include "peridot/lib/convert/convert.h"

namespace ledger {
namespace {

class LongHistorySyncTest : public IntegrationTest {
 protected:
  std::unique_ptr<TestSyncStateWatcher> WatchPageSyncState(PagePtr* page) {
    auto watcher = std::make_unique<TestSyncStateWatcher>();

    Status status = Status::INTERNAL_ERROR;
    auto waiter = NewWaiter();
    (*page)->SetSyncStateWatcher(
        watcher->NewBinding(),
        callback::Capture(waiter->GetCallback(), &status));
    if (!waiter->RunUntilCalled()) {
      return nullptr;
    }
    if (status != Status::OK) {
      return nullptr;
    }
    return watcher;
  }

  bool WaitUntilSyncIsIdle(TestSyncStateWatcher* watcher) {
    return RunLoopUntil([watcher] {
      return watcher->Equals(SyncState::IDLE, SyncState::IDLE);
    });
  }
};

TEST_P(LongHistorySyncTest, SyncLongHistory) {
  PageId page_id;
  Status status = Status::INTERNAL_ERROR;

  // Create the first instance and write the page entries.
  auto instance1 = NewLedgerAppInstance();
  auto page1 = instance1->GetTestPage();
  auto page1_state_watcher = WatchPageSyncState(&page1);
  ASSERT_TRUE(page1_state_watcher);
  const int commit_history_length = 500;
  // Overwrite one key N times, creating N implicit commits.
  for (int i = 0; i < commit_history_length; i++) {
    // TODO(ppi): switch to using a StatusWaiter to wait in parallel on all
    // puts, once this does not crash w/ ZX_ERR_SHOULD_WAIT in the test loop
    // dispatcher.
    auto put_waiter = NewWaiter();
    page1->Put(convert::ToArray("iteration"),
               convert::ToArray(std::to_string(i)),
               callback::Capture(put_waiter->GetCallback(), &status));
    ASSERT_TRUE(put_waiter->RunUntilCalled());
    ASSERT_EQ(Status::OK, status);
  }
  // Wait until the commits are uploaded.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page1_state_watcher.get()));

  // Retrieve the page ID so that we can later connect to the same page from
  // another app instance.
  auto waiter = NewWaiter();
  page1->GetId(callback::Capture(waiter->GetCallback(), &page_id));
  ASSERT_TRUE(waiter->RunUntilCalled());

  // Create the second instance, connect to the same page and download the
  // data.
  auto instance2 = NewLedgerAppInstance();
  auto page2 = instance2->GetPage(fidl::MakeOptional(page_id), Status::OK);
  auto page2_state_watcher = WatchPageSyncState(&page2);
  ASSERT_TRUE(page2_state_watcher);
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));

  PageSnapshotPtr snapshot;
  waiter = NewWaiter();
  page2->GetSnapshot(snapshot.NewRequest(), fidl::VectorPtr<uint8_t>::New(0),
                     nullptr,
                     callback::Capture(waiter->GetCallback(), &status));
  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  std::unique_ptr<InlinedValue> inlined_value;

  waiter = NewWaiter();
  snapshot->GetInline(
      convert::ToArray("iteration"),
      callback::Capture(waiter->GetCallback(), &status, &inlined_value));
  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  ASSERT_TRUE(inlined_value);
  const int last_iteration = commit_history_length - 1;
  ASSERT_EQ(std::to_string(last_iteration),
            convert::ToString(inlined_value->value));

  // Verify that the sync state of the second page connection eventually becomes
  // idle.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));
}

INSTANTIATE_TEST_SUITE_P(
    LongHistorySyncTest, LongHistorySyncTest,
    ::testing::ValuesIn(GetLedgerAppInstanceFactoryBuilders()));

}  // namespace
}  // namespace ledger
