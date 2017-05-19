// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <thread>

#include <mx/channel.h>

#include "apps/bluetooth/lib/common/byte_buffer.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/synchronization/thread_checker.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/tasks/message_loop_handler.h"

namespace bluetooth {
namespace hci {

class CommandPacket;

namespace test {

// Abstract base for implementing a fake HCI controller endpoint. This can directly send
// ACL data and event packets on request and forward outgoing ACL data packets to subclass
// implementations.
class FakeControllerBase : ::mtl::MessageLoopHandler {
 public:
  FakeControllerBase(mx::channel cmd_channel, mx::channel acl_data_channel);
  ~FakeControllerBase() override;

  // Kicks off the FakeController thread and message loop and starts processing transactions.
  // |debug_name| will be assigned as the name of the thread.
  void Start();

  // Stops the message loop and thread.
  void Stop();

  // Sends the given packet over this FakeController's command channel endpoint.
  void SendCommandChannelPacket(const common::ByteBuffer& packet);

  // Sends the given packet over this FakeController's ACL data channel endpoint.
  void SendACLDataChannelPacket(const common::ByteBuffer& packet);

  // Immediately closes the command channel endpoint.
  void CloseCommandChannel();

  // Immediately closes the ACL data channel endpoint.
  void CloseACLDataChannel();

  // Returns true if Start() has been called without a call to Stop().
  bool IsStarted() const { return static_cast<bool>(task_runner_); }

 protected:
  // Getters for our channel endpoints.
  const mx::channel& command_channel() const { return cmd_channel_; }
  const mx::channel& acl_data_channel() const { return acl_channel_; }

  // Called when there is an incoming command packet.
  virtual void OnCommandPacketReceived(const CommandPacket& command_packet) = 0;

  // Called when there is an outgoing ACL data packet.
  virtual void OnACLDataPacketReceived(const common::ByteBuffer& acl_data_packet) = 0;

 private:
  // ::mtl::MessageLoopHandler overrides
  void OnHandleReady(mx_handle_t handle, mx_signals_t pending) override;

  // Read and handle packets received over the channels.
  void HandleCommandPacket();
  void HandleACLPacket();

  // Used to assert that certain public functions are only called on the creation thread.
  ftl::ThreadChecker thread_checker_;

  mx::channel cmd_channel_;
  mx::channel acl_channel_;
  std::thread thread_;
  ftl::RefPtr<ftl::TaskRunner> task_runner_;
  mtl::MessageLoop::HandlerKey cmd_handler_key_;
  mtl::MessageLoop::HandlerKey acl_handler_key_;

  FTL_DISALLOW_COPY_AND_ASSIGN(FakeControllerBase);
};

}  // namespace test
}  // namespace hci
}  // namespace bluetooth
