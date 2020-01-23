// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/developer/debug/shared/zx_status_definitions.h"
#include "src/developer/debug/zxdb/client/mock_remote_api.h"
#include "src/developer/debug/zxdb/console/console_test.h"

namespace zxdb {

namespace {

// This just saves breakpoint add notifications. Tests will need to manually issue these callbacks.
class BreakpointRemoteAPI : public MockRemoteAPI {
 public:
  // RemoteAPI implementation.
  void AddOrChangeBreakpoint(
      const debug_ipc::AddOrChangeBreakpointRequest& request,
      fit::callback<void(const Err&, debug_ipc::AddOrChangeBreakpointReply)> cb) {
    last_request = request;
    last_cb = std::move(cb);
  }

  std::optional<debug_ipc::AddOrChangeBreakpointRequest> last_request;
  fit::callback<void(const Err&, debug_ipc::AddOrChangeBreakpointReply)> last_cb;
};

class VerbsBreakpointTest : public ConsoleTest {
 public:
  BreakpointRemoteAPI* breakpoint_remote_api() { return breakpoint_remote_api_; }

 private:
  // RemoteAPITest overrides.
  std::unique_ptr<RemoteAPI> GetRemoteAPIImpl() override {
    auto remote_api = std::make_unique<BreakpointRemoteAPI>();
    breakpoint_remote_api_ = remote_api.get();
    return remote_api;
  }

  BreakpointRemoteAPI* breakpoint_remote_api_ = nullptr;  // Owned by the session.
};

}  // namespace

TEST_F(VerbsBreakpointTest, Break) {
  // Process starts out as running. Make an expression breakpoint.
  console().ProcessInputLine("break \"*0x1230 + 4\"");

  // Validate the set request.
  ASSERT_TRUE(breakpoint_remote_api()->last_request);
  ASSERT_EQ(1u, breakpoint_remote_api()->last_request->breakpoint.locations.size());
  EXPECT_EQ(0x1234u, breakpoint_remote_api()->last_request->breakpoint.locations[0].address);

  // The breakpoint info should be immediately printed even though the backend has not replied.
  auto event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Created Breakpoint 1 @ 0x1234\n", event.output.AsString());

  // Issue the success callback from the backend. Nothing should be printed.
  ASSERT_TRUE(breakpoint_remote_api()->last_cb);
  breakpoint_remote_api()->last_cb(Err(), debug_ipc::AddOrChangeBreakpointReply());
  EXPECT_FALSE(console().HasOutputEvent());

  // Make a new process that's not running and then a breakpoint.
  console().ProcessInputLine("process new");
  console().FlushOutputEvents();
  console().ProcessInputLine("break SomePendingFunc");

  // It should give a pending message.
  event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ(
      "Created Breakpoint 2 @ SomePendingFunc\n"
      "Pending: No current matches for location. It will be matched against new\n"
      "         processes and shared libraries.\n",
      event.output.AsString());
}

// This is a more end-to-end-type test that tests that breakpoints that hit backend errors issue
// the proper notification and those notifications are caught and printed out on the screen.
TEST_F(VerbsBreakpointTest, TransportError) {
  // Create a breakpoint.
  console().ProcessInputLine("break 0x1234");
  auto event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Created Breakpoint 1 @ 0x1234\n", event.output.AsString());

  // Issue the callback with a transport error.
  ASSERT_TRUE(breakpoint_remote_api()->last_cb);
  breakpoint_remote_api()->last_cb(Err("Some transport error."),
                                   debug_ipc::AddOrChangeBreakpointReply());

  // The ConsoleContext should have printed out the error.
  event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ(
      "Error updating Breakpoint 1 @ 0x1234\n"
      "Some transport error.",
      event.output.AsString());
}

TEST_F(VerbsBreakpointTest, BackendError) {
  // Create a breakpoint.
  console().ProcessInputLine("break 0x2345");
  auto event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Created Breakpoint 1 @ 0x2345\n", event.output.AsString());

  // Issue the callback with a backend error.
  ASSERT_TRUE(breakpoint_remote_api()->last_cb);
  debug_ipc::AddOrChangeBreakpointReply reply;
  reply.status = debug_ipc::kZxErrBadHandle;
  breakpoint_remote_api()->last_cb(Err(), reply);

  // The ConsoleContext should have printed out the error.
  event = console().GetOutputEvent();
  ASSERT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ(
      "Error updating Breakpoint 1 @ 0x2345\n"
      "System reported error -11 (ZX_ERR_BAD_HANDLE)",
      event.output.AsString());
}

}  // namespace zxdb
