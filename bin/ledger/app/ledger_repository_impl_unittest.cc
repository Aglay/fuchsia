// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/ledger_repository_impl.h"

#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/macros.h>
#include <lib/fxl/strings/string_view.h>
#include <lib/gtest/test_loop_fixture.h>

#include "gtest/gtest.h"
#include "peridot/bin/ledger/app/ledger_repository_factory_impl.h"
#include "peridot/bin/ledger/storage/public/types.h"
#include "peridot/bin/ledger/testing/test_with_environment.h"
#include "peridot/lib/scoped_tmpfs/scoped_tmpfs.h"

namespace ledger {
namespace {

class FakeDiskCleanupManager : public DiskCleanupManager {
 public:
  FakeDiskCleanupManager() {}
  ~FakeDiskCleanupManager() override {}

  void set_on_empty(fit::closure on_empty_callback) override {}

  bool IsEmpty() override { return true; }

  void OnPageOpened(fxl::StringView ledger_name,
                    storage::PageIdView page_id) override {}

  void OnPageClosed(fxl::StringView ledger_name,
                    storage::PageIdView page_id) override {}

  void TryCleanUp(fit::function<void(Status)> callback) override {
    // Do not call the callback directly.
    cleanup_callback = std::move(callback);
  }

  fit::function<void(Status)> cleanup_callback;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(FakeDiskCleanupManager);
};

class LedgerRepositoryImplTest : public TestWithEnvironment {
 public:
  LedgerRepositoryImplTest() {
    auto fake_page_eviction_manager =
        std::make_unique<FakeDiskCleanupManager>();
    disk_cleanup_manager_ = fake_page_eviction_manager.get();

    repository_ = std::make_unique<LedgerRepositoryImpl>(
        DetachedPath(tmpfs_.root_fd()), &environment_, nullptr, nullptr,
        std::move(fake_page_eviction_manager));
  }

  ~LedgerRepositoryImplTest() override {}

 protected:
  scoped_tmpfs::ScopedTmpFS tmpfs_;
  std::unique_ptr<LedgerRepositoryImpl> repository_;
  FakeDiskCleanupManager* disk_cleanup_manager_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryImplTest);
};

TEST_F(LedgerRepositoryImplTest, DiskCleanUpError) {
  // Make a first call to DiskCleanUp.
  bool callback_called1 = false;
  Status status1;
  repository_->DiskCleanUp(
      callback::Capture(callback::SetWhenCalled(&callback_called1), &status1));

  // Make a second one before the first one has finished.
  bool callback_called2 = false;
  Status status2;
  repository_->DiskCleanUp(
      callback::Capture(callback::SetWhenCalled(&callback_called2), &status2));

  // Make sure both of them start running.
  RunLoopUntilIdle();

  // Only the second one should terminate with ILLEGAL_STATE status.
  EXPECT_FALSE(callback_called1);
  EXPECT_TRUE(callback_called2);
  EXPECT_EQ(Status::ILLEGAL_STATE, status2);

  // Call the callback and expect to see an ok status for the first one.
  disk_cleanup_manager_->cleanup_callback(Status::OK);
  RunLoopUntilIdle();
  EXPECT_TRUE(callback_called1);
  EXPECT_EQ(Status::OK, status1);
}

}  // namespace
}  // namespace ledger
