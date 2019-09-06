// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hw_breakpointer_helpers.h"

// This is a self contained binary that is meant to be run *manually*. This is the smallest code
// that can be used to reproduce a HW breakpoint exception.
// This is meant to be able to test the functionality of zircon without having to go throught the
// hassle of having the whole debugger context around.

namespace {

// Test Cases ======================================================================================

// BreakOnFunction ---------------------------------------------------------------------------------
//
// 1. Create a thread that will loop forever, continually calling a particular
//    function.
// 2. Suspend that thread.
// 3. Install a HW breakpoint through zx_thread_write_state.
// 4. Resume the thread.
// 5. Wait for some time for the exception. If the exception never happened, it
//    means that Zircon is not doing the right thing.

// This is the function that we will set up the breakpoint to.
using HWBreakpointTestCaseFunctionToBeCalled = int (*)(int);
int __NO_INLINE FunctionToBreakpointOn1(int c) { return c + c; }
int __NO_INLINE FunctionToBreakpointOn2(int c) { return c + c; }
int __NO_INLINE FunctionToBreakpointOn3(int c) { return c + c; }
int __NO_INLINE FunctionToBreakpointOn4(int c) { return c + c; }
int __NO_INLINE FunctionToBreakpointOn5(int c) { return c + c; }

// This is the code that the new thread will run.
// It's meant to be an eternal loop.
int BreakOnFunctionThreadFunction(void* user) {
  auto* thread_setup = reinterpret_cast<ThreadSetup*>(user);

  // We signal the test harness that we are here.
  thread_setup->event.signal(kHarnessToThread, kThreadToHarness);

  // We wait now for the harness to tell us we can continue.
  CHECK_OK(thread_setup->event.wait_one(kHarnessToThread, zx::time::infinite(), nullptr));

  PRINT("Got signaled by harness.");

  int counter = 1;
  while (thread_setup->test_running) {
    auto* function_to_call =
        reinterpret_cast<HWBreakpointTestCaseFunctionToBeCalled>(thread_setup->user);
    FXL_DCHECK(function_to_call);

    // We use write to avoid deadlocking with the outside libc calls.
    write(1, kBeacon, sizeof(kBeacon));
    counter = function_to_call(counter);
    zx::nanosleep(zx::deadline_after(zx::sec(1)));
  }

  return 0;
}

int BreakOnFunctionTestCase() {
  PRINT("Running HW breakpoint when calling a function test.");

  // The functions to be called sequentially by the test.
  HWBreakpointTestCaseFunctionToBeCalled breakpoint_functions[] = {
    FunctionToBreakpointOn1,
    FunctionToBreakpointOn2,
    FunctionToBreakpointOn3,
    FunctionToBreakpointOn4,
    FunctionToBreakpointOn5,
  };

  auto thread_setup = CreateTestSetup(BreakOnFunctionThreadFunction);


  zx::port port;
  CHECK_OK(zx::port::create(0, &port));

  zx::channel exception_channel;
  CHECK_OK(thread_setup->thread.create_exception_channel(0, &exception_channel));

  WaitAsyncOnExceptionChannel(port, exception_channel);

  Exception exception = {};

  for (size_t i = 0; i < ARRAY_SIZE(breakpoint_functions); i++) {
    // If this is the first iteration, we don't resume the exception.
    if (i > 0u) {
      WaitAsyncOnExceptionChannel(port, exception_channel);
      ResumeException(thread_setup->thread, std::move(exception));
    }

    auto* breakpoint_function = breakpoint_functions[i];

    // Pass in the function to call as extra data.
    thread_setup->user = reinterpret_cast<void*>(breakpoint_function);

    // Install the breakpoint.
    uint64_t breakpoint_address = reinterpret_cast<uint64_t>(breakpoint_function);
    InstallHWBreakpoint(thread_setup->thread, breakpoint_address);

    // Tell the thread to continue.
    thread_setup->event.signal(kThreadToHarness, kHarnessToThread);

    // We wait until we receive an exception.
    exception = WaitForException(port, exception_channel);

    FXL_DCHECK(exception.info.type == ZX_EXCP_HW_BREAKPOINT);
    PRINT("Hit HW breakpoint %zu on 0x%zx", i, exception.pc);

    // Remove the breakpoint.
    InstallHWBreakpoint(thread_setup->thread, 0);

  }

  // Tell the thread to exit.
  thread_setup->test_running = false;
  ResumeException(thread_setup->thread, std::move(exception));

  return 0;
}

// Channel messaging -------------------------------------------------------------------------------
//
// 1. Thread writes a set of messages into the channel then closes its endpoint.
// 2. The main thread will wait until the channel has been closed.
// 3. It will then read all the messages from it.

int ChannelMessagingThreadFunction(void* user) {
  ThreadSetup* thread_setup = reinterpret_cast<ThreadSetup*>(user);

  // We signal the test harness that we are here.
  thread_setup->event.signal(kHarnessToThread, kThreadToHarness);

  // We wait now for the harness to tell us we can continue.
  CHECK_OK(thread_setup->event.wait_one(kHarnessToThread, zx::time::infinite(), nullptr));

  zx::channel* channel = reinterpret_cast<zx::channel*>(thread_setup->user);

  constexpr char kMsg[] = "Hello, World!";

  for (int i = 0; i < 10; i++) {
    CHECK_OK(channel->write(0, kMsg, sizeof(kMsg), nullptr, 0));
    PRINT("Added message %d.", i);
  }

  channel->reset();
  PRINT("Closed channel.");

  return 0;
}

int ChannelMessagingTestCase() {
  PRINT("Running channel messaging.");

  zx::channel mine, theirs;
  CHECK_OK(zx::channel::create(0, &mine, &theirs));

  auto thread_setup = CreateTestSetup(ChannelMessagingThreadFunction, &theirs);

  // Tell the thread to continue.
  thread_setup->event.signal(kThreadToHarness, kHarnessToThread);

  // Wait for peer closed.
  CHECK_OK(mine.wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), nullptr));

  // Start reading from the channel.
  int read_count = 0;
  char buf[1024] = {};
  while (true) {
    zx_status_t status = mine.read_etc(0, buf, nullptr, sizeof(buf), 0, nullptr, nullptr);
    if (status == ZX_OK) {
      PRINT("Read message %d: %s", read_count++, buf);
    } else {
      PRINT("No more messages (status: %s).", zx_status_get_string(status));
      break;
    }
  }

  thread_setup->test_running = false;

  return 0;
}

}  // namespace

// Main --------------------------------------------------------------------------------------------

struct TestCase {
  using TestFunction = int(*)();

  std::string name = nullptr;
  std::string description = nullptr;
  TestFunction test_function = nullptr;
};

TestCase kTestCases[] = {
    {"hw_breakpoints", "Call multiple HW breakpoints on different functions.",
     BreakOnFunctionTestCase},

    {"channel_calls",
     "Send multiple messages over a channel call and read from it after it is closed.",
     ChannelMessagingTestCase},
};

void PrintUsage() {
  printf("Usage: hw_breakpointer <TEST CASE>\n");
  printf("Test cases are:\n");
  for (auto& test_case : kTestCases) {
    printf("- %s: %s\n", test_case.name.c_str(), test_case.description.c_str());
  }
  fflush(stdout);
}

TestCase::TestFunction GetTestCase(std::string test_name) {
  for (auto& test_case : kTestCases) {
    if (test_name == test_case.name)
      return test_case.test_function;
  }

  return nullptr;
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    PrintUsage();
    return 1;
  }

  const char* test_name = argv[1];
  auto* test_function = GetTestCase(test_name);
  if (!test_function) {
    printf("Unknown test case %s\n", test_name);
    PrintUsage();
    return 1;
  }

  return test_function();
}
