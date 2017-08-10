// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/bluetooth/lib/common/run_task_sync.h"

#include "gtest/gtest.h"

#include "lib/ftl/synchronization/sleep.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/threading/create_thread.h"

namespace bluetooth {
namespace common {
namespace {

TEST(RunTaskSyncTest, RunTaskSync) {
  constexpr int64_t kSleepTimeMs = 10;
  constexpr int kLoopCount = 50;

  ftl::RefPtr<ftl::TaskRunner> task_runner;
  std::thread thrd = mtl::CreateThread(&task_runner, "RunTaskSyncTest thread");

  for (int i = 0; i < kLoopCount; ++i) {
    bool callback_run = false;
    auto cb = [&callback_run] {
      ftl::SleepFor(ftl::TimeDelta::FromMilliseconds(kSleepTimeMs));
      callback_run = true;
    };

    RunTaskSync(cb, task_runner);
    EXPECT_TRUE(callback_run);
  }

  task_runner->PostTask([] { mtl::MessageLoop::GetCurrent()->QuitNow(); });
  if (thrd.joinable()) thrd.join();
}

}  // namespace
}  // namespace common
}  // namespace bluetooth
