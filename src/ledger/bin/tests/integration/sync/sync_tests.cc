// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/callback/capture.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fsl/vmo/vector.h>

#include "peridot/lib/convert/convert.h"
#include "peridot/lib/rng/test_random.h"
#include "src/ledger/bin/testing/data_generator.h"
#include "src/ledger/bin/tests/integration/integration_test.h"
#include "src/ledger/bin/tests/integration/sync/test_sync_state_watcher.h"

namespace ledger {
namespace {

class SyncIntegrationTest : public IntegrationTest {
 protected:
  std::unique_ptr<TestSyncStateWatcher> WatchPageSyncState(PagePtr* page) {
    auto watcher = std::make_unique<TestSyncStateWatcher>();

    Status status = Status::INTERNAL_ERROR;
    auto loop_waiter = NewWaiter();
    (*page)->SetSyncStateWatcher(
        watcher->NewBinding(),
        callback::Capture(loop_waiter->GetCallback(), &status));
    EXPECT_TRUE(loop_waiter->RunUntilCalled());
    EXPECT_EQ(Status::OK, status);

    return watcher;
  }

  bool WaitUntilSyncIsIdle(TestSyncStateWatcher* watcher) {
    return RunLoopUntil([watcher] {
      return watcher->Equals(SyncState::IDLE, SyncState::IDLE);
    });
  }
};

// Verifies that a new page entry is correctly synchronized between two Ledger
// app instances.
//
// In this test the app instances connect to the cloud one after the other: the
// first instance uploads data to the cloud and shuts down, and only after that
// the second instance is created and connected.
TEST_P(SyncIntegrationTest, SerialConnection) {
  PageId page_id;
  Status status;

  // Create the first instance and write the page entry.
  auto instance1 = NewLedgerAppInstance();
  auto page1 = instance1->GetTestPage();
  auto page1_state_watcher = WatchPageSyncState(&page1);
  auto loop_waiter = NewWaiter();
  page1->Put(convert::ToArray("Hello"), convert::ToArray("World"),
             callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());

  // Retrieve the page ID so that we can later connect to the same page from
  // another app instance.
  ASSERT_EQ(Status::OK, status);
  loop_waiter = NewWaiter();
  page1->GetId(callback::Capture(loop_waiter->GetCallback(), &page_id));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());

  // Wait until the sync state becomes idle.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page1_state_watcher.get()));

  // Create the second instance, connect to the same page and download the
  // data.
  auto instance2 = NewLedgerAppInstance();
  auto page2 = instance2->GetPage(fidl::MakeOptional(page_id));
  auto page2_state_watcher = WatchPageSyncState(&page2);
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));

  PageSnapshotPtr snapshot;
  loop_waiter = NewWaiter();
  page2->GetSnapshot(snapshot.NewRequest(), fidl::VectorPtr<uint8_t>::New(0),
                     nullptr,
                     callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  std::unique_ptr<InlinedValue> inlined_value;

  loop_waiter = NewWaiter();
  snapshot->GetInline(
      convert::ToArray("Hello"),
      callback::Capture(loop_waiter->GetCallback(), &status, &inlined_value));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  ASSERT_TRUE(inlined_value);
  ASSERT_EQ("World", convert::ToString(inlined_value->value));

  // Verify that the sync state of the second page connection eventually becomes
  // idle.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));
}

// Verifies that a new page entry is correctly synchronized between two Ledger
// app instances.
//
// In this test the app instances connect to the cloud concurrently: the second
// instance is already connected when the first instance writes the entry.
TEST_P(SyncIntegrationTest, ConcurrentConnection) {
  auto instance1 = NewLedgerAppInstance();
  auto instance2 = NewLedgerAppInstance();

  auto page1 = instance1->GetTestPage();
  auto page1_state_watcher = WatchPageSyncState(&page1);
  PageId page_id;
  auto loop_waiter = NewWaiter();
  page1->GetId(callback::Capture(loop_waiter->GetCallback(), &page_id));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  auto page2 = instance2->GetPage(fidl::MakeOptional(page_id));
  auto page2_state_watcher = WatchPageSyncState(&page2);
  // Wait until the sync on the second device is idle and record the number of
  // state updates.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));
  int page2_initial_state_change_count =
      page2_state_watcher->state_change_count;

  Status status;
  loop_waiter = NewWaiter();
  page1->Put(convert::ToArray("Hello"), convert::ToArray("World"),
             callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);

  // Wait until page1 finishes uploading the changes.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page1_state_watcher.get()));

  // Note that we cannot just wait for the sync to become idle on the second
  // instance, as it might still be idle upon the first check because the device
  // hasn't yet received the remote notification about new commits. This is why
  // we also check that another state change notification was delivered.
  EXPECT_TRUE(
      RunLoopUntil([&page2_state_watcher, page2_initial_state_change_count] {
        return page2_state_watcher->state_change_count >
                   page2_initial_state_change_count &&
               page2_state_watcher->Equals(SyncState::IDLE, SyncState::IDLE);
      }));

  PageSnapshotPtr snapshot;
  loop_waiter = NewWaiter();
  page2->GetSnapshot(snapshot.NewRequest(), fidl::VectorPtr<uint8_t>::New(0),
                     nullptr,
                     callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  std::unique_ptr<InlinedValue> inlined_value;
  loop_waiter = NewWaiter();
  snapshot->GetInline(
      convert::ToArray("Hello"),
      callback::Capture(loop_waiter->GetCallback(), &status, &inlined_value));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  ASSERT_TRUE(inlined_value);
  ASSERT_EQ("World", convert::ToString(inlined_value->value));

  // Verify that the sync states of page2 eventually become idle.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));
}

// Verify that we download eager values in full, even if parts of these values
// were already present on disk.
//
// In this test, we connect to the page concurrently. The first connection
// uploads a big object as a LAZY value, then the second one fetches a part of
// it. After that, the first connection re-uploads the same value, but with an
// EAGER priority. When the second connection receives the changes, we verify
// that the object is fully present on disk and can be retrieved by calling Get.
TEST_P(SyncIntegrationTest, LazyToEagerTransition) {
  auto instance1 = NewLedgerAppInstance();
  auto instance2 = NewLedgerAppInstance();

  auto page1 = instance1->GetTestPage();
  auto page1_state_watcher = WatchPageSyncState(&page1);
  PageId page_id;
  auto loop_waiter = NewWaiter();
  page1->GetId(callback::Capture(loop_waiter->GetCallback(), &page_id));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  auto page2 = instance2->GetPage(fidl::MakeOptional(page_id));
  auto page2_state_watcher = WatchPageSyncState(&page2);
  // Wait until the sync on the second device is idle and record the number of
  // state updates.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));
  int page2_initial_state_change_count =
      page2_state_watcher->state_change_count;

  rng::TestRandom rng(0);
  DataGenerator generator = DataGenerator(&rng);

  Status status;
  std::vector<uint8_t> key = convert::ToArray("Hello");
  std::vector<uint8_t> big_value = generator.MakeValue(2 * 65536 + 1).take();
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromVector(big_value, &vmo));
  ReferencePtr reference;
  loop_waiter = NewWaiter();
  page1->CreateReferenceFromBuffer(
      std::move(vmo).ToTransport(),
      callback::Capture(loop_waiter->GetCallback(), &status, &reference));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  loop_waiter = NewWaiter();
  page1->PutReference(key, *reference, Priority::LAZY,
                      callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);

  // Wait until page1 finishes uploading the changes.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page1_state_watcher.get()));

  EXPECT_TRUE(
      RunLoopUntil([&page2_state_watcher, page2_initial_state_change_count] {
        return page2_state_watcher->state_change_count >
                   page2_initial_state_change_count &&
               page2_state_watcher->Equals(SyncState::IDLE, SyncState::IDLE);
      }));

  PageSnapshotPtr snapshot;
  loop_waiter = NewWaiter();
  page2->GetSnapshot(snapshot.NewRequest(), fidl::VectorPtr<uint8_t>::New(0),
                     nullptr,
                     callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  fuchsia::mem::BufferPtr buffer;
  loop_waiter = NewWaiter();
  // Lazy value is not downloaded eagerly.
  snapshot->Get(
      convert::ToArray("Hello"),
      callback::Capture(loop_waiter->GetCallback(), &status, &buffer));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::NEEDS_FETCH, status);

  loop_waiter = NewWaiter();
  // Fetch only a small part.
  snapshot->FetchPartial(
      convert::ToArray("Hello"), 0, 10,
      callback::Capture(loop_waiter->GetCallback(), &status, &buffer));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  ASSERT_EQ(buffer->size, 10u);

  // Change priority to eager, re-upload.
  loop_waiter = NewWaiter();
  page1->PutReference(key, *reference, Priority::EAGER,
                      callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);

  page2_initial_state_change_count = page2_state_watcher->state_change_count;
  // Wait until page1 finishes uploading the changes.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page1_state_watcher.get()));

  EXPECT_TRUE(
      RunLoopUntil([&page2_state_watcher, page2_initial_state_change_count] {
        return page2_state_watcher->state_change_count >
                   page2_initial_state_change_count &&
               page2_state_watcher->Equals(SyncState::IDLE, SyncState::IDLE);
      }));

  loop_waiter = NewWaiter();
  page2->GetSnapshot(snapshot.NewRequest(), fidl::VectorPtr<uint8_t>::New(0),
                     nullptr,
                     callback::Capture(loop_waiter->GetCallback(), &status));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);
  loop_waiter = NewWaiter();

  // Now Get succeeds, as the value is no longer lazy.
  snapshot->Get(
      convert::ToArray("Hello"),
      callback::Capture(loop_waiter->GetCallback(), &status, &buffer));
  ASSERT_TRUE(loop_waiter->RunUntilCalled());
  ASSERT_EQ(Status::OK, status);

  // Check the content.
  ASSERT_EQ(buffer->size, big_value.size());
  std::vector<uint8_t> buffer_data;
  ASSERT_TRUE(fsl::VectorFromVmo(*buffer, &buffer_data));
  ASSERT_EQ(buffer_data, big_value);

  // Verify that the sync states of page2 eventually become idle.
  EXPECT_TRUE(WaitUntilSyncIsIdle(page2_state_watcher.get()));
}

INSTANTIATE_TEST_SUITE_P(
    SyncIntegrationTest, SyncIntegrationTest,
    ::testing::ValuesIn(GetLedgerAppInstanceFactoryBuilders()));

}  // namespace
}  // namespace ledger
