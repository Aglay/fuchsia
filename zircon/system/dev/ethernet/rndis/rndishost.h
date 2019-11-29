// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_ETHERNET_RNDIS_RNDISHOST_H_
#define ZIRCON_SYSTEM_DEV_ETHERNET_RNDIS_RNDISHOST_H_

#include <stdint.h>
#include <zircon/compiler.h>

#include <ddk/protocol/usb.h>
#include <ddktl/device.h>
#include <ddktl/protocol/ethernet.h>
#include <fbl/mutex.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

// clang-format off

// USB subclass and protocol for binding
#define RNDIS_SUBCLASS                  0x01
#define RNDIS_PROTOCOL                  0x03

#define RNDIS_MAJOR_VERSION             0x00000001
#define RNDIS_MINOR_VERSION             0x00000000
#define RNDIS_MAX_XFER_SIZE             0x00004000

// Messages
#define RNDIS_PACKET_MSG                0x00000001
#define RNDIS_INITIALIZE_MSG            0x00000002
#define RNDIS_QUERY_MSG                 0x00000004
#define RNDIS_SET_MSG                   0x00000005
#define RNDIS_INITIALIZE_CMPLT          0x80000002
#define RNDIS_QUERY_CMPLT               0x80000004
#define RNDIS_SET_CMPLT                 0x80000005

// Statuses
#define RNDIS_STATUS_SUCCESS            0x00000000
#define RNDIS_STATUS_FAILURE            0xC0000001
#define RNDIS_STATUS_INVALID_DATA       0xC0010015
#define RNDIS_STATUS_NOT_SUPPORTED      0xC00000BB
#define RNDIS_STATUS_MEDIA_CONNECT      0x4001000B
#define RNDIS_STATUS_MEDIA_DISCONNECT   0x4001000C

// OIDs
#define OID_802_3_PERMANENT_ADDRESS     0x01010101
#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010e
#define OID_GEN_PHYSICAL_MEDIUM         0x00010202

// Filter options
#define RNDIS_PACKET_TYPE_DIRECTED        0x00000001
#define RNDIS_PACKET_TYPE_MULTICAST       0x00000002
#define RNDIS_PACKET_TYPE_ALL_MULTICAST   0x00000004
#define RNDIS_PACKET_TYPE_BROADCAST       0x00000008
#define RNDIS_PACKET_TYPE_SOURCE_ROUTING  0x00000010
#define RNDIS_PACKET_TYPE_PROMISCUOUS     0x00000020
#define RNDIS_PACKET_TYPE_SMT             0x00000040
#define RNDIS_PACKET_TYPE_ALL_LOCAL       0x00000080
#define RNDIS_PACKET_TYPE_GROUP           0x00001000
#define RNDIS_PACKET_TYPE_ALL_FUNCTIONAL  0x00002000
#define RNDIS_PACKET_TYPE_FUNCTIONAL      0x00004000
#define RNDIS_PACKET_TYPE_MAC_FRAME       0x00008000

#define RNDIS_BUFFER_SIZE 1024
#define RNDIS_QUERY_BUFFER_OFFSET 20
#define RNDIS_CONTROL_TIMEOUT ZX_SEC(5)

// clang-format on

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
} __PACKED rndis_header;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t status;
} __PACKED rndis_header_complete;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t major_version;
  uint32_t minor_version;
  uint32_t max_xfer_size;
} __PACKED rndis_init;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t status;
  uint32_t major_version;
  uint32_t minor_version;
  uint32_t device_flags;
  uint32_t medium;
  uint32_t max_packers_per_xfer;
  uint32_t max_xfer_size;
  uint32_t packet_alignment;
  uint32_t reserved0;
  uint32_t reserved1;
} __PACKED rndis_init_complete;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t oid;
  uint32_t info_buffer_length;
  uint32_t info_buffer_offset;
  uint32_t reserved;
} __PACKED rndis_query;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t status;
  uint32_t info_buffer_length;
  uint32_t info_buffer_offset;
} __PACKED rndis_query_complete;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t oid;
  uint32_t info_buffer_length;
  uint32_t info_buffer_offset;
  uint32_t reserved;
} __PACKED rndis_set;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t request_id;
  uint32_t status;
  // TODO: Figure out why these fields aren't send in the reply. Maybe the
  // Pixel I'm testing with implements the spec badly?
  // uint32_t info_buffer_length;
  // uint32_t info_buffer_offset;
} __PACKED rndis_set_complete;

typedef struct {
  uint32_t msg_type;
  uint32_t msg_length;
  uint32_t data_offset;
  uint32_t data_length;
  uint32_t oob_data_offset;
  uint32_t oob_data_length;
  uint32_t num_oob_elements;
  uint32_t per_packet_info_offset;
  uint32_t per_packet_info_length;
  uint32_t reserved0;
  uint32_t reserved1;
} __PACKED rndis_packet_header;

class RndisHost;

using RndisHostType = ddk::Device<RndisHost, ddk::UnbindableNew>;

class RndisHost : public RndisHostType,
                  public ddk::EthernetImplProtocol<RndisHost, ddk::base_protocol> {
 public:
  explicit RndisHost(zx_device_t* parent, uint8_t control_intf, uint8_t bulk_in_addr,
                     uint8_t bulk_out_addr, const usb::UsbDevice& usb);

  void DdkUnbindNew(ddk::UnbindTxn txn);
  void DdkRelease();

  zx_status_t InitBuffers();
  zx_status_t AddDevice();

  zx_status_t EthernetImplQuery(uint32_t options, ethernet_info_t* info);
  void EthernetImplStop();
  zx_status_t EthernetImplStart(const ethernet_ifc_protocol_t* ifc_);
  void EthernetImplQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                           ethernet_impl_queue_tx_callback completion_cb, void* cookie);
  zx_status_t EthernetImplSetParam(uint32_t param, int32_t value, const void* data,
                                   size_t data_size);
  void EthernetImplGetBti(zx::bti* out_bti);

 private:
  zx_status_t StartThread();

  void WriteComplete(usb_request_t* request);
  void ReadComplete(usb_request_t* request);

  void Recv(usb_request_t* request);

  zx_status_t Command(void* buf);

  usb::UsbDevice usb_;

  uint8_t mac_addr_[ETH_MAC_SIZE];
  uint8_t control_intf_;
  uint32_t next_request_id_;
  uint32_t mtu_;

  uint8_t bulk_in_addr_;
  uint8_t bulk_out_addr_;

  list_node_t free_read_reqs_;
  list_node_t free_write_reqs_;

  uint64_t rx_endpoint_delay_;  // wait time between 2 recv requests
  uint64_t tx_endpoint_delay_;  // wait time between 2 transmit requests

  // Interface to the ethernet layer.
  ethernet_ifc_protocol_t ifc_;

  thrd_t thread_;
  bool thread_started_ = false;
  size_t parent_req_size_;

  fbl::Mutex mutex_;
};

#endif  // ZIRCON_SYSTEM_DEV_ETHERNET_RNDIS_RNDISHOST_H_
