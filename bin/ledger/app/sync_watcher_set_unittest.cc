// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/sync_watcher_set.h"

#include <algorithm>
#include <string>

#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/gtest/test_with_message_loop.h"

namespace ledger {
namespace {

class SyncWatcherSetTest : public gtest::TestWithMessageLoop {
 public:
  SyncWatcherSetTest() {}
  ~SyncWatcherSetTest() override {}

 protected:
  void SetUp() override { ::testing::Test::SetUp(); }

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(SyncWatcherSetTest);
};

class SyncWatcherImpl : public SyncWatcher {
 public:
  explicit SyncWatcherImpl(f1dl::InterfaceRequest<SyncWatcher> request)
      : binding_(this, std::move(request)) {}
  void SyncStateChanged(SyncState download_status,
                        SyncState upload_status,
                        const SyncStateChangedCallback& callback) override {
    download_states.push_back(download_status);
    upload_states.push_back(upload_status);
    callback();
    fsl::MessageLoop::GetCurrent()->PostQuitTask();
  }

  std::vector<SyncState> download_states;
  std::vector<SyncState> upload_states;

 private:
  f1dl::Binding<SyncWatcher> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SyncWatcherImpl);
};

TEST_F(SyncWatcherSetTest, OneWatcher) {
  SyncWatcherSet watcher_set;
  SyncWatcherPtr watcher_ptr;

  SyncWatcherImpl impl(watcher_ptr.NewRequest());

  watcher_set.Notify(cloud_sync::DOWNLOAD_BACKLOG,
                     cloud_sync::UPLOAD_WAIT_REMOTE_DOWNLOAD);

  watcher_set.AddSyncWatcher(std::move(watcher_ptr));

  RunLoopUntilIdle();

  ASSERT_EQ(1u, impl.download_states.size());
  EXPECT_EQ(SyncState::IN_PROGRESS, *impl.download_states.rbegin());
  ASSERT_EQ(1u, impl.upload_states.size());
  EXPECT_EQ(SyncState::PENDING, *impl.upload_states.rbegin());

  watcher_set.Notify(cloud_sync::DOWNLOAD_PERMANENT_ERROR,
                     cloud_sync::UPLOAD_IDLE);

  RunLoopUntilIdle();

  ASSERT_EQ(2u, impl.download_states.size());
  EXPECT_EQ(SyncState::ERROR, *impl.download_states.rbegin());
  ASSERT_EQ(2u, impl.upload_states.size());
  EXPECT_EQ(SyncState::IDLE, *impl.upload_states.rbegin());
}

TEST_F(SyncWatcherSetTest, TwoWatchers) {
  SyncWatcherSet watcher_set;

  SyncWatcherPtr watcher_ptr1;
  SyncWatcherImpl impl1(watcher_ptr1.NewRequest());
  watcher_set.AddSyncWatcher(std::move(watcher_ptr1));

  RunLoopUntilIdle();
  EXPECT_EQ(1u, impl1.download_states.size());
  EXPECT_EQ(SyncState::IDLE, *impl1.download_states.rbegin());
  EXPECT_EQ(1u, impl1.upload_states.size());
  EXPECT_EQ(SyncState::IDLE, *impl1.upload_states.rbegin());

  SyncWatcherPtr watcher_ptr2;
  SyncWatcherImpl impl2(watcher_ptr2.NewRequest());
  watcher_set.AddSyncWatcher(std::move(watcher_ptr2));

  RunLoopUntilIdle();
  EXPECT_EQ(1u, impl2.download_states.size());
  EXPECT_EQ(SyncState::IDLE, *impl2.download_states.rbegin());
  EXPECT_EQ(1u, impl2.upload_states.size());
  EXPECT_EQ(SyncState::IDLE, *impl2.upload_states.rbegin());

  watcher_set.Notify(cloud_sync::DOWNLOAD_IN_PROGRESS,
                     cloud_sync::UPLOAD_WAIT_REMOTE_DOWNLOAD);

  RunLoopUntilIdle();

  ASSERT_EQ(2u, impl1.download_states.size());
  EXPECT_EQ(SyncState::IN_PROGRESS, *impl1.download_states.rbegin());
  ASSERT_EQ(2u, impl1.upload_states.size());
  EXPECT_EQ(SyncState::PENDING, *impl1.upload_states.rbegin());

  ASSERT_EQ(2u, impl2.download_states.size());
  EXPECT_EQ(SyncState::IN_PROGRESS, *impl2.download_states.rbegin());
  ASSERT_EQ(2u, impl2.upload_states.size());
  EXPECT_EQ(SyncState::PENDING, *impl2.upload_states.rbegin());
}

}  // namespace
}  // namespace ledger
