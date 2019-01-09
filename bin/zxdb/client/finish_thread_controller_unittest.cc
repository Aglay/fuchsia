// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/finish_thread_controller.h"
#include "garnet/bin/zxdb/client/process.h"
#include "garnet/bin/zxdb/client/thread.h"
#include "garnet/bin/zxdb/client/thread_controller_test.h"
#include "garnet/bin/zxdb/client/thread_impl_test_support.h"
#include "garnet/bin/zxdb/common/err.h"
#include "gtest/gtest.h"

namespace zxdb {

namespace {

constexpr uint64_t kInitialAddress = 0x12345678;
constexpr uint64_t kInitialBase = 0x1000;
constexpr uint64_t kReturnAddress = 0x34567890;
constexpr uint64_t kReturnBase = 0x1010;

class FinishThreadControllerTest : public ThreadControllerTest {
 public:
  // Creates a break notification with two stack frames using the constants
  // above.
  debug_ipc::NotifyException MakeBreakNotification() {
    debug_ipc::NotifyException n;

    n.process_koid = process()->GetKoid();
    n.type = debug_ipc::NotifyException::Type::kSoftware;
    n.thread.koid = thread()->GetKoid();
    n.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
    n.thread.stack_amount = debug_ipc::ThreadRecord::StackAmount::kMinimal;
    n.thread.frames.emplace_back(kInitialAddress, kInitialBase, kInitialBase);
    n.thread.frames.emplace_back(kReturnAddress, kReturnBase, kReturnBase);

    return n;
  }
};

}  // namespace

TEST_F(FinishThreadControllerTest, Finish) {
  // Notify of thread stop.
  auto break_notification = MakeBreakNotification();
  InjectException(break_notification);

  // Supply three frames for when the thread requests them: the top one (of the
  // stop above), the one we'll return to, and the one before that (so the
  // fingerprint of the one to return to can be computed). This stack value
  // should be larger than above (stack grows downward).
  debug_ipc::ThreadStatusReply expected_reply;
  // Copy previous frames and add to it.
  expected_reply.record = break_notification.thread;
  expected_reply.record.stack_amount =
      debug_ipc::ThreadRecord::StackAmount::kFull;
  expected_reply.record.frames.emplace_back(kReturnAddress, kReturnBase,
                                            kReturnBase);
  mock_remote_api()->set_thread_status_reply(expected_reply);

  auto frames = thread()->GetStack().GetFrames();

  EXPECT_EQ(0, mock_remote_api()->breakpoint_add_count());
  Err out_err;
  thread()->ContinueWith(std::make_unique<FinishThreadController>(
                             FinishThreadController::FromFrame(), frames[0]),
                         [&out_err](const Err& err) {
                           out_err = err;
                           debug_ipc::MessageLoop::Current()->QuitNow();
                         });
  loop().Run();

  TestThreadObserver thread_observer(thread());

  // Finish should have added a temporary breakpoint at the return address.
  // The particulars of this may change with the implementation, but it's worth
  // testing to make sure the breakpoints are all hooked up to the stepping
  // properly.
  ASSERT_EQ(1, mock_remote_api()->breakpoint_add_count());
  ASSERT_EQ(kReturnAddress, mock_remote_api()->last_breakpoint_address());
  ASSERT_EQ(0, mock_remote_api()->breakpoint_remove_count());

  // Simulate a hit of the breakpoint. This stack pointer is too small
  // (indicating a recursive call) so it should not trigger.
  break_notification.thread.frames.clear();
  break_notification.thread.frames.emplace_back(
      kReturnAddress, kInitialBase - 0x100, kInitialBase - 0x100);
  break_notification.hit_breakpoints.emplace_back();
  break_notification.hit_breakpoints[0].breakpoint_id =
      mock_remote_api()->last_breakpoint_id();
  InjectException(break_notification);
  EXPECT_FALSE(thread_observer.got_stopped());

  // Simulate a breakpoint hit with a lower BP. This should trigger a thread
  // stop.
  break_notification.thread.frames[0].sp = kReturnBase;
  break_notification.thread.frames[0].bp = kReturnBase;
  InjectException(break_notification);
  EXPECT_TRUE(thread_observer.got_stopped());
  EXPECT_EQ(1, mock_remote_api()->breakpoint_remove_count());
}

// Tests "finish" at the bottom stack frame. Normally there's a stack frame
// with an IP of 0 below the last "real" stack frame.
TEST_F(FinishThreadControllerTest, BottomStackFrame) {
  // Notify of thread stop. Here we have the 0th frame of the current
  // location, and a null frame.
  auto break_notification = MakeBreakNotification();
  break_notification.thread.frames[1] = debug_ipc::StackFrame(0, 0, 0);
  InjectException(break_notification);

  // The backtrace reply gives the same two frames since that's all there is
  // (the Thread doesn't know until it requests them).
  debug_ipc::ThreadStatusReply expected_reply;
  expected_reply.record = break_notification.thread;
  expected_reply.record.stack_amount =
      debug_ipc::ThreadRecord::StackAmount::kFull;
  mock_remote_api()->set_thread_status_reply(expected_reply);

  auto frames = thread()->GetStack().GetFrames();

  EXPECT_EQ(0, mock_remote_api()->breakpoint_add_count());
  Err out_err;
  thread()->ContinueWith(std::make_unique<FinishThreadController>(
                             FinishThreadController::FromFrame(), frames[0]),
                         [&out_err](const Err& err) {
                           out_err = err;
                           debug_ipc::MessageLoop::Current()->QuitNow();
                         });
  loop().Run();

  TestThreadObserver thread_observer(thread());

  // Since the return address is null, we should not have attempted to create
  // a breakpoint, and the thread should have been resumed.
  ASSERT_EQ(0, mock_remote_api()->breakpoint_add_count());
  ASSERT_EQ(1, mock_remote_api()->resume_count());
}

}  // namespace zxdb
