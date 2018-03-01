// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/callback/capture.h"
#include "lib/fsl/vmo/strings.h"
#include "peridot/bin/ledger/tests/integration/integration_test.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace integration {
namespace {

class SyncIntegrationTest : public IntegrationTest {
 protected:
  ::testing::AssertionResult GetEntries(
      ledger::Page* page,
      f1dl::Array<ledger::EntryPtr>* entries) {
    ledger::PageSnapshotPtr snapshot;
    ledger::Status status;
    page->GetSnapshot(snapshot.NewRequest(), nullptr, nullptr,
                      callback::Capture(MakeQuitTask(), &status));
    if (RunLoopWithTimeout() || status != ledger::Status::OK) {
      return ::testing::AssertionFailure() << "Unable to retrieve a snapshot";
    }
    entries->resize(0);
    f1dl::Array<uint8_t> token = nullptr;
    f1dl::Array<uint8_t> next_token = nullptr;
    do {
      f1dl::Array<ledger::EntryPtr> new_entries;
      snapshot->GetEntries(nullptr, std::move(token),
                           callback::Capture(MakeQuitTask(), &status,
                                             &new_entries, &next_token));
      if (RunLoopWithTimeout() || status != ledger::Status::OK) {
        return ::testing::AssertionFailure() << "Unable to retrieve entries";
      }
      for (auto& entry : new_entries) {
        entries->push_back(std::move(entry));
      }
      token = std::move(next_token);
    } while (token);
    return ::testing::AssertionSuccess();
  }
};

TEST_F(SyncIntegrationTest, SerialConnection) {
  auto instance1 = NewLedgerAppInstance();
  auto page = instance1->GetTestPage();
  ledger::Status status;
  page->Put(convert::ToArray("Hello"), convert::ToArray("World"),
            callback::Capture(MakeQuitTask(), &status));
  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);
  f1dl::Array<uint8_t> page_id;
  page->GetId(callback::Capture(MakeQuitTask(), &page_id));
  ASSERT_FALSE(RunLoopWithTimeout());

  auto instance2 = NewLedgerAppInstance();
  page = instance2->GetPage(page_id, ledger::Status::OK);
  EXPECT_TRUE(RunLoopUntilWithTimeout([this, &page] {
    f1dl::Array<ledger::EntryPtr> entries;
    if (!GetEntries(page.get(), &entries)) {
      return true;
    }
    return !entries.empty();
  }));

  ledger::PageSnapshotPtr snapshot;
  page->GetSnapshot(snapshot.NewRequest(), nullptr, nullptr,
                    callback::Capture(MakeQuitTask(), &status));
  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);
  f1dl::Array<uint8_t> value;
  snapshot->GetInline(convert::ToArray("Hello"),
                      callback::Capture(MakeQuitTask(), &status, &value));
  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);
  ASSERT_EQ("World", convert::ToString(value));
}

TEST_F(SyncIntegrationTest, ConcurrentConnection) {
  auto instance1 = NewLedgerAppInstance();
  auto instance2 = NewLedgerAppInstance();

  auto page1 = instance1->GetTestPage();
  f1dl::Array<uint8_t> page_id;
  page1->GetId(callback::Capture(MakeQuitTask(), &page_id));
  ASSERT_FALSE(RunLoopWithTimeout());
  auto page2 = instance2->GetPage(page_id, ledger::Status::OK);

  ledger::Status status;
  page1->Put(convert::ToArray("Hello"), convert::ToArray("World"),
             callback::Capture(MakeQuitTask(), &status));
  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);

  EXPECT_TRUE(RunLoopUntilWithTimeout([this, &page2] {
    f1dl::Array<ledger::EntryPtr> entries;
    if (!GetEntries(page2.get(), &entries)) {
      return true;
    }
    return !entries.empty();
  }));

  ledger::PageSnapshotPtr snapshot;
  page2->GetSnapshot(snapshot.NewRequest(), nullptr, nullptr,
                     callback::Capture(MakeQuitTask(), &status));
  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);
  f1dl::Array<uint8_t> value;
  snapshot->GetInline(convert::ToArray("Hello"),
                      callback::Capture(MakeQuitTask(), &status, &value));
  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);
  ASSERT_EQ("World", convert::ToString(value));
}

}  // namespace
}  // namespace integration
}  // namespace test
