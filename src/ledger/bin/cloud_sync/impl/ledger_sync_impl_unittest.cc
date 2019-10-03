// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/ledger_sync_impl.h"

#include <lib/gtest/test_loop_fixture.h>

#include "peridot/lib/scoped_tmpfs/scoped_tmpfs.h"
#include "src/ledger/bin/cloud_sync/impl/testing/test_cloud_provider.h"
#include "src/ledger/bin/cloud_sync/impl/testing/test_page_storage.h"
#include "src/ledger/bin/encryption/fake/fake_encryption_service.h"
#include "src/ledger/bin/testing/test_with_environment.h"
#include "src/lib/callback/capture.h"
#include "src/lib/callback/set_when_called.h"

namespace cloud_sync {

namespace {

class TestSyncStateWatcher : public SyncStateWatcher {
 public:
  TestSyncStateWatcher() = default;
  ~TestSyncStateWatcher() override{};

  void Notify(SyncStateContainer sync_state) override {
    if (!states.empty() && sync_state == *states.rbegin()) {
      return;
    }
    states.push_back(sync_state);
  }

  std::vector<SyncStateContainer> states;
};

class LedgerSyncImplTest : public ledger::TestWithEnvironment {
 public:
  LedgerSyncImplTest()
      : cloud_provider_(cloud_provider_ptr_.NewRequest()), encryption_service_(dispatcher()) {
    user_config_.user_directory = ledger::DetachedPath(tmpfs_.root_fd());
    user_config_.cloud_provider = std::move(cloud_provider_ptr_);

    ledger_sync_ = std::make_unique<LedgerSyncImpl>(&environment_, &user_config_,
                                                    &encryption_service_, "test_app_id", nullptr);
  }
  ~LedgerSyncImplTest() override {}

 protected:
  scoped_tmpfs::ScopedTmpFS tmpfs_;
  std::unique_ptr<LedgerSyncImpl> ledger_sync_;
  cloud_provider::CloudProviderPtr cloud_provider_ptr_;
  TestCloudProvider cloud_provider_;
  encryption::FakeEncryptionService encryption_service_;
  UserConfig user_config_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerSyncImplTest);
};

TEST_F(LedgerSyncImplTest, CreatePageSync) {
  auto page_storage = std::make_unique<TestPageStorage>(dispatcher());
  page_storage->page_id_to_return = "test_page";
  bool called;
  storage::Status status;
  std::unique_ptr<PageSync> page_sync;
  ledger_sync_->CreatePageSync(
      page_storage.get(), page_storage.get(),
      callback::Capture(callback::SetWhenCalled(&called), &status, &page_sync));
  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_EQ(status, storage::Status::OK);
  EXPECT_TRUE(page_sync);
  EXPECT_TRUE(page_sync->IsPaused());
  ASSERT_EQ(cloud_provider_.page_ids_requested.size(), 1u);
  // Page id must be obfuscated in the cloud.
  EXPECT_NE(cloud_provider_.page_ids_requested[0u], page_storage->page_id_to_return);
}

}  // namespace

}  // namespace cloud_sync
