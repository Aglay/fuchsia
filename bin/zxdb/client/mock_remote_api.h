// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "garnet/bin/zxdb/client/remote_api.h"
#include "garnet/lib/debug_ipc/protocol.h"

namespace zxdb {

// A mock for RemoteAPI that saves messages and sends replies.
//
// Not all of the messages are handled here. Only the ones that are needed by
// the tests that use this mock are necessary. The default implementation of
// RemoteAPI will assert for calls that aren't overridden, so if you get one
// you should implement it here.
class MockRemoteAPI : public RemoteAPI {
 public:
  MockRemoteAPI();
  ~MockRemoteAPI();

  // Resume.
  int resume_count() const { return resume_count_; }

  // Thread status.
  void set_thread_status_reply(const debug_ipc::ThreadStatusReply& reply) {
    thread_status_reply_ = reply;
  }

  // Breakpoints.
  int breakpoint_add_count() const { return breakpoint_add_count_; }
  int breakpoint_remove_count() const { return breakpoint_remove_count_; }
  const debug_ipc::AddOrChangeBreakpointRequest& last_breakpoint_add() const {
    return last_breakpoint_add_;
  }
  uint64_t last_breakpoint_id() const {
    return last_breakpoint_add_.breakpoint.breakpoint_id;
  }
  uint64_t last_breakpoint_address() const {
    if (last_breakpoint_add_.breakpoint.locations.empty())
      return 0;
    return last_breakpoint_add_.breakpoint.locations[0].address;
  }

  const debug_ipc::WriteRegistersRequest& last_write_registers() const {
    return last_write_registers_;
  }

  // RemoteAPI implementation.
  void AddOrChangeBreakpoint(
      const debug_ipc::AddOrChangeBreakpointRequest& request,
      std::function<void(const Err&, debug_ipc::AddOrChangeBreakpointReply)> cb)
      override;
  void RemoveBreakpoint(
      const debug_ipc::RemoveBreakpointRequest& request,
      std::function<void(const Err&, debug_ipc::RemoveBreakpointReply)> cb)
      override;
  void ThreadStatus(
      const debug_ipc::ThreadStatusRequest& request,
      std::function<void(const Err&, debug_ipc::ThreadStatusReply)> cb)
      override;
  void Resume(
      const debug_ipc::ResumeRequest& request,
      std::function<void(const Err&, debug_ipc::ResumeReply)> cb) override;
  void WriteRegisters(
      const debug_ipc::WriteRegistersRequest& request,
      std::function<void(const Err&, debug_ipc::WriteRegistersReply)> cb)
      override;

 private:
  debug_ipc::ThreadStatusReply thread_status_reply_;

  int resume_count_ = 0;
  int breakpoint_add_count_ = 0;
  int breakpoint_remove_count_ = 0;
  debug_ipc::AddOrChangeBreakpointRequest last_breakpoint_add_;
  debug_ipc::WriteRegistersRequest last_write_registers_;
};

}  // namespace zxdb
