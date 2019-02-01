// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/memory_monitor/monitor.h"
#include "gtest/gtest.h"
#include "lib/component/cpp/testing/test_with_context.h"

namespace memory {
namespace test {

using namespace fuchsia::memory;
using namespace memory;

class MonitorUnitTest : public component::testing::TestWithContext {
 protected:
  MonitorUnitTest()
      : monitor_(std::make_unique<Monitor>(TakeContext(), fxl::CommandLine{},
                                           dispatcher())) {}

  void TearDown() override {
    monitor_.reset();
    TestWithContext::TearDown();
  }

  MonitorPtr monitor() {
    MonitorPtr monitor;
    controller().outgoing_public_services().ConnectToService(
        monitor.NewRequest());
    return monitor;
  }

private:
  std::unique_ptr<Monitor> monitor_;
};

class WatcherForTest : public Watcher {
 public:
  WatcherForTest(fit::function<void(uint64_t free_bytes)> on_change)
      : on_change_(std::move(on_change)) {}

  void OnChange(Stats stats) override {
    on_change_(stats.free_bytes);
  }

  void AddBinding(fidl::InterfaceRequest<Watcher> request) {
    bindings_.AddBinding(this, std::move(request));
  }

private:
  fidl::BindingSet<Watcher> bindings_;
  fit::function<void(uint64_t free_bytes)> on_change_;
};

TEST_F(MonitorUnitTest, FreeBytes) {
  bool got_free = false;
  WatcherForTest watcher([&got_free](uint64_t free_bytes) {
    got_free = true;
  });
  WatcherPtr watcher_ptr;
  watcher.AddBinding(watcher_ptr.NewRequest());

  monitor()->Watch(watcher_ptr.Unbind());
  RunLoopUntilIdle();
  EXPECT_TRUE(got_free);
}

}  // namespace test
}  // namespace memory
