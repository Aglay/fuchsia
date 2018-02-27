// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/cloud_sync/impl/user_sync_impl.h"

#include <utility>

#include "garnet/lib/gtest/test_with_message_loop.h"
#include "lib/fxl/files/file.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/cloud_sync/impl/testing/test_cloud_provider.h"
#include "peridot/bin/ledger/encryption/fake/fake_encryption_service.h"
#include "peridot/lib/backoff/backoff.h"
#include "peridot/lib/backoff/testing/test_backoff.h"

namespace cloud_sync {

namespace {

class TestSyncStateWatcher : public SyncStateWatcher {
 public:
  TestSyncStateWatcher() {}
  ~TestSyncStateWatcher() override{};

  void Notify(SyncStateContainer /*sync_state*/) override {}
};

class UserSyncImplTest : public gtest::TestWithMessageLoop {
 public:
  UserSyncImplTest()
      : environment_(message_loop_.task_runner()),
        cloud_provider_(cloud_provider_ptr_.NewRequest()),
        encryption_service_(message_loop_.task_runner()) {
    UserConfig user_config;
    user_config.user_directory = tmp_dir.path();
    user_config.cloud_provider = std::move(cloud_provider_ptr_);

    auto backoff = std::make_unique<backoff::TestBackoff>();
    backoff->SetOnGetNext([this] {
      // Make RunLoopUntilIdle() return once a backoff is requested, to avoid an
      // infinite loop.
      message_loop_.PostQuitTask();
    });

    user_sync_ = std::make_unique<UserSyncImpl>(
        &environment_, std::move(user_config),
        std::move(backoff), &sync_state_watcher_,
        [this] { on_version_mismatch_calls_++; });
  }
  ~UserSyncImplTest() override {}

 protected:
  bool SetFingerprintFile(std::string content) {
    std::string fingerprint_path = user_sync_->GetFingerprintPath();
    return files::WriteFile(fingerprint_path, content.data(), content.size());
  }

  files::ScopedTempDir tmp_dir;
  ledger::Environment environment_;
  cloud_provider::CloudProviderPtr cloud_provider_ptr_;
  TestCloudProvider cloud_provider_;
  std::unique_ptr<UserSyncImpl> user_sync_;
  encryption::FakeEncryptionService encryption_service_;
  TestSyncStateWatcher sync_state_watcher_;

  int on_version_mismatch_calls_ = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(UserSyncImplTest);
};

// Verifies that the mismatch callback is called if the fingerprint appears to
// be erased from the cloud.
TEST_F(UserSyncImplTest, CloudCheckErased) {
  ASSERT_TRUE(SetFingerprintFile("some-value"));
  cloud_provider_.device_set.status_to_return =
      cloud_provider::Status::NOT_FOUND;
  EXPECT_EQ(0, on_version_mismatch_calls_);
  user_sync_->Start();
  RunLoopUntilIdle();
  EXPECT_EQ(1, on_version_mismatch_calls_);
}

// Verifies that if the version checker reports that cloud is compatible, upload
// is enabled in LedgerSync.
TEST_F(UserSyncImplTest, CloudCheckOk) {
  ASSERT_TRUE(SetFingerprintFile("some-value"));
  cloud_provider_.device_set.status_to_return = cloud_provider::Status::OK;
  EXPECT_EQ(0, on_version_mismatch_calls_);
  user_sync_->Start();

  auto ledger_a = user_sync_->CreateLedgerSync("app-id", &encryption_service_);
  auto ledger_a_ptr = static_cast<LedgerSyncImpl*>(ledger_a.get());
  EXPECT_FALSE(ledger_a_ptr->IsUploadEnabled());
  RunLoopUntilIdle();
  EXPECT_TRUE(ledger_a_ptr->IsUploadEnabled());
  EXPECT_EQ(0, on_version_mismatch_calls_);
  EXPECT_EQ("some-value", cloud_provider_.device_set.checked_fingerprint);

  // Verify that newly created LedgerSyncs also have the upload enabled.
  auto ledger_b = user_sync_->CreateLedgerSync("app-id", &encryption_service_);
  auto ledger_b_ptr = static_cast<LedgerSyncImpl*>(ledger_b.get());
  EXPECT_TRUE(ledger_b_ptr->IsUploadEnabled());
}

// Verifies that if there is no fingerprint file, it is created and set in the
// cloud.
TEST_F(UserSyncImplTest, CloudCheckSet) {
  EXPECT_FALSE(files::IsFile(user_sync_->GetFingerprintPath()));
  cloud_provider_.device_set.status_to_return = cloud_provider::Status::OK;
  EXPECT_EQ(0, on_version_mismatch_calls_);
  user_sync_->Start();

  auto ledger = user_sync_->CreateLedgerSync("app-id", &encryption_service_);
  auto ledger_ptr = static_cast<LedgerSyncImpl*>(ledger.get());
  EXPECT_FALSE(ledger_ptr->IsUploadEnabled());
  RunLoopUntilIdle();
  EXPECT_TRUE(ledger_ptr->IsUploadEnabled());
  EXPECT_EQ(0, on_version_mismatch_calls_);
  EXPECT_FALSE(cloud_provider_.device_set.set_fingerprint.empty());

  // Verify that the fingerprint file was created.
  EXPECT_TRUE(files::IsFile(user_sync_->GetFingerprintPath()));
}

// Verifies that the cloud watcher for the fingerprint is set and triggers the
// mismatch callback when cloud erase is detected.
TEST_F(UserSyncImplTest, WatchErase) {
  ASSERT_TRUE(SetFingerprintFile("some-value"));
  cloud_provider_.device_set.status_to_return = cloud_provider::Status::OK;
  user_sync_->Start();

  RunLoopUntilIdle();
  EXPECT_TRUE(cloud_provider_.device_set.set_watcher.is_bound());
  EXPECT_EQ("some-value", cloud_provider_.device_set.watched_fingerprint);
  EXPECT_EQ(0, on_version_mismatch_calls_);

  cloud_provider_.device_set.set_watcher->OnCloudErased();
  RunLoopUntilIdle();
  EXPECT_EQ(1, on_version_mismatch_calls_);
}

// Verifies that setting the cloud watcher for is retried on network errors.
TEST_F(UserSyncImplTest, WatchRetry) {
  ASSERT_TRUE(SetFingerprintFile("some-value"));
  cloud_provider_.device_set.set_watcher_status_to_return =
      cloud_provider::Status::NETWORK_ERROR;
  user_sync_->Start();

  RunLoopUntilIdle();
  EXPECT_EQ(1, cloud_provider_.device_set.set_watcher_calls);
}

}  // namespace

}  // namespace cloud_sync
