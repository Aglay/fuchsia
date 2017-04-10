// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <unordered_map>

#include "apps/bluetooth/lib/common/device_address.h"
#include "apps/bluetooth/lib/hci/command_channel.h"
#include "apps/bluetooth/lib/hci/connection.h"
#include "apps/bluetooth/lib/hci/hci.h"
#include "apps/bluetooth/lib/hci/transport.h"
#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/memory/ref_ptr.h"
#include "lib/mtl/tasks/message_loop.h"

namespace hci_acl_test {

// This is a LE connection tester that works directly against the HCI transport classes. This tester
// performs the following:
//
//   - Initialize HCI transport.
//   - Obtain buffer size information from the controller.
//   - Create direct LE connection to a remote device with a public BD_ADDR.
//   - Listen to ACL packets and respond the ATT protocol requests without any L2CAP state
//     management.
class LEConnectionTest final {
 public:
  LEConnectionTest();
  bool Run(ftl::UniqueFD hci_dev, const bluetooth::common::DeviceAddress& dst_addr);

 private:
  // Initializes the data channel and sends a LE connection request to |dst_addr_|. Exits the
  // run loop if an error occurs.
  void InitializeDataChannelAndCreateConnection(size_t max_data_len, size_t max_num_packets,
                                                size_t le_max_data_len, size_t le_max_num_packets);

  // Called after the connection has been successfully established. Sends 3 times the maximum number
  // of LE packets that can be stored in the controller's buffers. Sends ATT protocol Handle-Value
  // notification PDUs.
  void SendNotifications();

  // Called when ACL data packets are received.
  void ACLDataRxCallback(bluetooth::common::DynamicByteBuffer rx_bytes);

  // Logs the given message and status code and exits the run loop.
  void LogErrorStatusAndQuit(const std::string& msg, bluetooth::hci::Status status);

  // Returns a status callback that can be used while sending commands.
  bluetooth::hci::CommandChannel::CommandStatusCallback GetStatusCallback(
      const std::string& command_name);

  // Logs the status code and exits the run loop.
  void StatusCallback(const std::string& command_name,
                      bluetooth::hci::CommandChannel::TransactionId id,
                      bluetooth::hci::Status status);

  std::unique_ptr<bluetooth::hci::Transport> hci_;
  mtl::MessageLoop message_loop_;
  bluetooth::common::DeviceAddress dst_addr_;
  bluetooth::hci::CommandChannel::EventHandlerId le_conn_complete_handler_id_;
  bluetooth::hci::CommandChannel::EventHandlerId disconn_handler_id_;
  std::unordered_map<bluetooth::hci::ConnectionHandle, ftl::RefPtr<bluetooth::hci::Connection>>
      conn_map_;

  FTL_DISALLOW_COPY_AND_ASSIGN(LEConnectionTest);
};

}  // namespace hci_acl_test
