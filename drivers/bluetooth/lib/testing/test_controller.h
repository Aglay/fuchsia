// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <queue>
#include <vector>

#include "apps/bluetooth/lib/common/byte_buffer.h"
#include "apps/bluetooth/lib/hci/hci.h"
#include "apps/bluetooth/lib/testing/fake_controller_base.h"
#include "lib/ftl/macros.h"

namespace bluetooth {
namespace testing {

// A CommandTransaction is used to set up an expectation for a command channel packet and the
// events that should be sent back in response to it.
class CommandTransaction final {
 public:
  CommandTransaction() = default;
  CommandTransaction(const common::ByteBuffer& expected,
                     const std::vector<const common::ByteBuffer*>& replies);

  // Move constructor and assignment operator.
  CommandTransaction(CommandTransaction&& other) = default;
  CommandTransaction& operator=(CommandTransaction&& other) = default;

 private:
  friend class TestController;

  bool HasMoreResponses() const;
  common::DynamicByteBuffer PopNextReply();

  common::DynamicByteBuffer expected_;
  std::queue<common::DynamicByteBuffer> replies_;

  FTL_DISALLOW_COPY_AND_ASSIGN(CommandTransaction);
};

// TestController allows unit tests to set up an expected sequence of HCI commands and any events
// that should be sent back in response. The code internally verifies each received HCI command
// using gtest ASSERT_* macros.
class TestController : public FakeControllerBase {
 public:
  TestController(mx::channel cmd_channel, mx::channel acl_data_channel);
  ~TestController() override;

  // Queues a transaction into the TestController's expected command queue. Each packet received
  // through the command channel endpoint will be verified against the next expected transaction in
  // the queue. A mismatch will cause a fatal assertion. On a match, TestController will send back
  // the replies provided in the transaction.
  void QueueCommandTransaction(CommandTransaction transaction);

  // Callback to invoke when a packet is received over the data channel.
  using DataCallback = std::function<void(const common::ByteBuffer& packet)>;
  void SetDataCallback(const DataCallback& callback, ftl::RefPtr<ftl::TaskRunner> task_runner);

 private:
  // FakeControllerBase overrides:
  void OnCommandPacketReceived(
      const common::PacketView<hci::CommandHeader>& command_packet) override;
  void OnACLDataPacketReceived(const common::ByteBuffer& acl_data_packet) override;

  std::queue<CommandTransaction> cmd_transactions_;
  DataCallback data_callback_;
  ftl::RefPtr<ftl::TaskRunner> data_task_runner_;

  FTL_DISALLOW_COPY_AND_ASSIGN(TestController);
};

}  // namespace testing
}  // namespace bluetooth
