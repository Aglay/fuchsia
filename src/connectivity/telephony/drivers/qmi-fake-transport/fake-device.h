// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_CONNECTIVITY_TELEPHONY_DRIVERS_QMI_FAKE_TRANSPORT_FAKE_DEVICE_H_
#define SRC_CONNECTIVITY_TELEPHONY_DRIVERS_QMI_FAKE_TRANSPORT_FAKE_DEVICE_H_
#include <zircon/compiler.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/test.h>

#define _ALL_SOURCE
#include <fuchsia/hardware/telephony/transport/llcpp/fidl.h>
#include <fuchsia/telephony/snoop/llcpp/fidl.h>
#include <threads.h>

// port info
#define CHANNEL_MSG 1
#define INTERRUPT_MSG 2
#define TERMINATE_MSG 3

namespace qmi_fake {

class Device : ::llcpp::fuchsia::hardware::telephony::transport::Qmi::Interface {
 public:
  Device(zx_device_t* device);

  zx_status_t FidlDispatch(fidl_msg_t* msg, fidl_txn_t* txn);
  zx_status_t Message(fidl_msg_t* msg, fidl_txn_t* txn);
  zx_status_t Bind();
  void Unbind();
  void Release();

  zx_status_t GetProtocol(uint32_t proto_id, void* out_proto);
  zx_status_t SetChannelToDevice(zx_handle_t transport);
  zx_status_t SetNetworkStatusToDevice(bool connected);
  zx_status_t SetSnoopChannelToDevice(zx_handle_t channel);
  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
  void ReplyQmiMsg(uint8_t* req, uint32_t req_size, uint8_t* resp, uint32_t resp_size);
  void SnoopQmiMsg(uint8_t* snoop_data, uint32_t snoop_data_len,
                   ::llcpp::fuchsia::telephony::snoop::Direction direction);
  zx_status_t CloseQmiChannel();
  zx_handle_t GetQmiChannel();
  zx_handle_t GetQmiChannelPort();
  zx_status_t SetAsyncWait();
  zx_status_t EventLoopCleanup();

 private:
  void SetChannel(::zx::channel transport, SetChannelCompleter::Sync completer) override;
  void SetNetwork(bool connected, SetNetworkCompleter::Sync completer) override;
  void SetSnoopChannel(::zx::channel interface, SetSnoopChannelCompleter::Sync completer) override;

  zx_handle_t qmi_channel_;
  zx_handle_t qmi_channel_port_;
  zx_handle_t snoop_channel_port_;
  zx_handle_t snoop_channel_;
  bool connected_;
  zx_device_t* parent_;
  zx_device_t* zxdev_;
  thrd_t fake_qmi_thread_;
};

}  // namespace qmi_fake

#endif  // SRC_CONNECTIVITY_TELEPHONY_DRIVERS_QMI_FAKE_TRANSPORT_FAKE_DEVICE_H_
