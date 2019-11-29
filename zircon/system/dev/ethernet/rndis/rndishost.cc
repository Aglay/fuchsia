// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rndishost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/hw/usb.h>
#include <zircon/hw/usb/cdc.h>
#include <zircon/listnode.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/ethernet.h>
#include <ddk/protocol/usb.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

#define READ_REQ_COUNT 8
#define WRITE_REQ_COUNT 4
#define ETH_HEADER_SIZE 4

#define ETHERNET_MAX_TRANSMIT_DELAY 100
#define ETHERNET_MAX_RECV_DELAY 100
#define ETHERNET_TRANSMIT_DELAY 10
#define ETHERNET_RECV_DELAY 10
#define ETHERNET_INITIAL_TRANSMIT_DELAY 0
#define ETHERNET_INITIAL_RECV_DELAY 0

static bool command_succeeded(const void* buf, uint32_t type, size_t length) {
  const auto* header = static_cast<const rndis_header_complete*>(buf);
  if (header->msg_type != type) {
    zxlogf(DEBUG1, "Bad type: Actual: %x, Expected: %x.\n", header->msg_type, type);
    return false;
  }
  if (header->msg_length != length) {
    zxlogf(DEBUG1, "Bad length: Actual: %u, Expected: %zu.\n", header->msg_length, length);
    return false;
  }
  if (header->status != RNDIS_STATUS_SUCCESS) {
    zxlogf(DEBUG1, "Bad status: %x.\n", header->status);
    return false;
  }
  return true;
}

zx_status_t RndisHost::Command(void* buf) {
  rndis_header* header = static_cast<rndis_header*>(buf);
  uint32_t request_id = next_request_id_++;
  header->request_id = request_id;

  zx_status_t status;
  status = usb_control_out(&usb_, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                           USB_CDC_SEND_ENCAPSULATED_COMMAND, 0, control_intf_,
                           RNDIS_CONTROL_TIMEOUT, buf, header->msg_length);

  if (status < 0) {
    return status;
  }

  status = usb_control_in(&usb_, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                          USB_CDC_GET_ENCAPSULATED_RESPONSE, 0, control_intf_,
                          RNDIS_CONTROL_TIMEOUT, buf, RNDIS_BUFFER_SIZE, nullptr);

  if (header->request_id != request_id) {
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  return status;
}

void RndisHost::Recv(usb_request_t* request) {
  size_t len = request->response.actual;

  void* read_data;
  zx_status_t status = usb_request_mmap(request, &read_data);
  if (status != ZX_OK) {
    zxlogf(ERROR, "rndishost receive: usb_request_mmap failed: %d\n", status);
    return;
  }

  while (len > sizeof(rndis_packet_header)) {
    rndis_packet_header* header = static_cast<rndis_packet_header*>(read_data);

    // The |data_offset| field contains the offset to the payload measured from the start of
    // the field itself.
    size_t data_offset = offsetof(rndis_packet_header, data_offset) + header->data_offset;

    if (header->msg_type != RNDIS_PACKET_MSG || len < header->msg_length ||
        len < data_offset + header->data_length) {
      zxlogf(DEBUG1, "rndis bad packet\n");
      return;
    }

    if (header->data_length == 0) {
      // No more data.
      return;
    }

    ethernet_ifc_recv(&ifc_, static_cast<uint8_t*>(read_data) + data_offset, header->data_length,
                      0);

    read_data = static_cast<uint8_t*>(read_data) + header->msg_length;
    len -= header->msg_length;
  }
}

void RndisHost::ReadComplete(usb_request_t* request) {
  if (request->response.status == ZX_ERR_IO_NOT_PRESENT) {
    usb_request_release(request);
    return;
  }

  fbl::AutoLock lock(&mutex_);
  if (request->response.status == ZX_ERR_IO_REFUSED) {
    zxlogf(TRACE, "rndis_read_complete usb_reset_endpoint\n");
    usb_reset_endpoint(&usb_, bulk_in_addr_);
  } else if (request->response.status == ZX_ERR_IO_INVALID) {
    zxlogf(TRACE,
           "rndis_read_complete Slowing down the requests by %d usec"
           " and resetting the recv endpoint\n",
           ETHERNET_RECV_DELAY);
    if (rx_endpoint_delay_ < ETHERNET_MAX_RECV_DELAY) {
      rx_endpoint_delay_ += ETHERNET_RECV_DELAY;
    }
    usb_reset_endpoint(&usb_, bulk_in_addr_);
  }
  if (request->response.status == ZX_OK && ifc_.ops) {
    Recv(request);
  } else {
    zxlogf(DEBUG1, "rndis read complete: bad status = %d\n", request->response.status);
  }

  // TODO: Only usb_request_queue if the device is online.
  zx_nanosleep(zx_deadline_after(ZX_USEC(rx_endpoint_delay_)));
  usb_request_complete_t complete = {
      .callback = [](void* arg, usb_request_t* request) -> void {
        static_cast<RndisHost*>(arg)->ReadComplete(request);
      },
      .ctx = this,
  };
  usb_request_queue(&usb_, request, &complete);
}

void RndisHost::WriteComplete(usb_request_t* request) {
  if (request->response.status == ZX_ERR_IO_NOT_PRESENT) {
    zxlogf(ERROR, "rndis_write_complete zx_err_io_not_present\n");
    usb_request_release(request);
    return;
  }

  fbl::AutoLock lock(&mutex_);
  if (request->response.status == ZX_ERR_IO_REFUSED) {
    zxlogf(TRACE, "rndishost usb_reset_endpoint\n");
    usb_reset_endpoint(&usb_, bulk_out_addr_);
  } else if (request->response.status == ZX_ERR_IO_INVALID) {
    zxlogf(TRACE,
           "rndis_write_complete Slowing down the requests by %d usec"
           " and resetting the transmit endpoint\n",
           ETHERNET_TRANSMIT_DELAY);
    if (tx_endpoint_delay_ < ETHERNET_MAX_TRANSMIT_DELAY) {
      tx_endpoint_delay_ += ETHERNET_TRANSMIT_DELAY;
    }
    usb_reset_endpoint(&usb_, bulk_out_addr_);
  }

  zx_status_t status = usb_req_list_add_tail(&free_write_reqs_, request, parent_req_size_);
  ZX_DEBUG_ASSERT(status == ZX_OK);
}

RndisHost::RndisHost(zx_device_t* parent, uint8_t control_intf, uint8_t bulk_in_addr,
                     uint8_t bulk_out_addr, usb_protocol_t usb)
    : RndisHostType(parent),
      usb_(usb),
      mac_addr_{},
      control_intf_(control_intf),
      next_request_id_(0),
      bulk_in_addr_(bulk_in_addr),
      bulk_out_addr_(bulk_out_addr),
      rx_endpoint_delay_(0),
      tx_endpoint_delay_(0),
      ifc_({}),
      thread_started_(false),
      parent_req_size_(usb_get_request_size(&usb)) {
  list_initialize(&free_read_reqs_);
  list_initialize(&free_write_reqs_);

  ifc_.ops = nullptr;
}

zx_status_t RndisHost::EthernetImplQuery(uint32_t options, ethernet_info_t* info) {
  if (options) {
    return ZX_ERR_INVALID_ARGS;
  }

  memset(info, 0, sizeof(*info));
  info->mtu = mtu_;
  memcpy(info->mac, mac_addr_, sizeof(mac_addr_));
  info->netbuf_size = sizeof(ethernet_netbuf_t);

  return ZX_OK;
}

void RndisHost::EthernetImplStop() {
  fbl::AutoLock lock(&mutex_);
  ifc_.ops = nullptr;
}

zx_status_t RndisHost::EthernetImplStart(const ethernet_ifc_protocol_t* ifc) {
  fbl::AutoLock lock(&mutex_);
  if (ifc_.ops) {
    return ZX_ERR_ALREADY_BOUND;
  }

  ifc_ = *ifc;
  // TODO: Check that the device is online before sending ETHERNET_STATUS_ONLINE.
  ethernet_ifc_status(&ifc_, ETHERNET_STATUS_ONLINE);
  return ZX_OK;
}

void RndisHost::EthernetImplQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                                    ethernet_impl_queue_tx_callback completion_cb, void* cookie) {
  size_t length = netbuf->data_size;
  const uint8_t* byte_data = static_cast<const uint8_t*>(netbuf->data_buffer);
  zx_status_t status = ZX_OK;

  fbl::AutoLock lock(&mutex_);

  usb_request_t* req = usb_req_list_remove_head(&free_write_reqs_, parent_req_size_);
  if (req == NULL) {
    zxlogf(TRACE, "rndishost dropped a packet\n");
    status = ZX_ERR_NO_RESOURCES;
    goto done;
  }

  static_assert(RNDIS_MAX_XFER_SIZE >= sizeof(rndis_packet_header));

  if (length > RNDIS_MAX_XFER_SIZE - sizeof(rndis_packet_header)) {
    zxlogf(TRACE, "rndishost attempted to send a packet that's too large.\n");
    status = usb_req_list_add_tail(&free_write_reqs_, req, parent_req_size_);
    ZX_DEBUG_ASSERT(status == ZX_OK);
    status = ZX_ERR_INVALID_ARGS;
    goto done;
  }

  {
    rndis_packet_header header;
    uint8_t* header_data = reinterpret_cast<uint8_t*>(&header);
    memset(header_data, 0, sizeof(rndis_packet_header));
    header.msg_type = RNDIS_PACKET_MSG;
    header.msg_length = static_cast<uint32_t>(sizeof(rndis_packet_header) + length);
    static_assert(offsetof(rndis_packet_header, data_offset) == 8);
    header.data_offset = sizeof(rndis_packet_header) - offsetof(rndis_packet_header, data_offset);
    header.data_length = static_cast<uint32_t>(length);

    usb_request_copy_to(req, header_data, sizeof(rndis_packet_header), 0);

    ssize_t bytes_copied = usb_request_copy_to(req, byte_data, length, sizeof(rndis_packet_header));
    req->header.length = sizeof(rndis_packet_header) + length;
    if (bytes_copied < 0) {
      zxlogf(ERROR, "rndishost: failed to copy data into send txn (error %zd)\n", bytes_copied);
      status = usb_req_list_add_tail(&free_write_reqs_, req, parent_req_size_);
      ZX_DEBUG_ASSERT(status == ZX_OK);
      goto done;
    }
  }
  zx_nanosleep(zx_deadline_after(ZX_USEC(tx_endpoint_delay_)));
  {
    usb_request_complete_t complete = {
        .callback = [](void* arg, usb_request_t* request) -> void {
          static_cast<RndisHost*>(arg)->WriteComplete(request);
        },
        .ctx = this,
    };
    usb_request_queue(&usb_, req, &complete);
  }

done:
  lock.release();
  completion_cb(cookie, status, netbuf);
}

void RndisHost::DdkUnbindNew(ddk::UnbindTxn txn) { txn.Reply(); }

void RndisHost::DdkRelease() {
  bool should_join;
  {
    fbl::AutoLock lock(&mutex_);
    should_join = thread_started_;
  }
  if (should_join) {
    thrd_join(thread_, NULL);
  }

  usb_request_t* txn;
  while ((txn = usb_req_list_remove_head(&free_read_reqs_, parent_req_size_)) != NULL) {
    usb_request_release(txn);
  }
  while ((txn = usb_req_list_remove_head(&free_write_reqs_, parent_req_size_)) != NULL) {
    usb_request_release(txn);
  }
}

zx_status_t RndisHost::EthernetImplSetParam(uint32_t param, int32_t value, const void* data,
                                            size_t data_size) {
  return ZX_ERR_NOT_SUPPORTED;
}

void RndisHost::EthernetImplGetBti(zx::bti* out_bti) {}

zx_status_t RndisHost::StartThread() {
  void* buf = malloc(RNDIS_BUFFER_SIZE);
  memset(buf, 0, RNDIS_BUFFER_SIZE);

  // Send an initialization message to the device.
  rndis_init* init = static_cast<rndis_init*>(buf);
  init->msg_type = RNDIS_INITIALIZE_MSG;
  init->msg_length = sizeof(rndis_init);
  init->major_version = RNDIS_MAJOR_VERSION;
  init->minor_version = RNDIS_MINOR_VERSION;
  init->max_xfer_size = RNDIS_MAX_XFER_SIZE;

  zx_status_t status = Command(buf);
  if (status < 0) {
    zxlogf(ERROR, "rndishost bad status on initial message. %d\n", status);
    goto fail;
  }
  {
    rndis_init_complete* init_cmplt = static_cast<rndis_init_complete*>(buf);
    if (!command_succeeded(buf, RNDIS_INITIALIZE_CMPLT, sizeof(*init_cmplt))) {
      zxlogf(ERROR, "rndishost initialization failed.\n");
      status = ZX_ERR_IO;
      goto fail;
    }
    mtu_ = init_cmplt->max_xfer_size;
  }

  {
    // Query the device for a MAC address.
    memset(buf, 0, RNDIS_BUFFER_SIZE);
    rndis_query* query = static_cast<rndis_query*>(buf);
    query->msg_type = RNDIS_QUERY_MSG;
    query->msg_length = sizeof(rndis_query) + 48;
    query->oid = OID_802_3_PERMANENT_ADDRESS;
    query->info_buffer_length = 48;
    query->info_buffer_offset = RNDIS_QUERY_BUFFER_OFFSET;
    status = Command(buf);
    if (status < 0) {
      zxlogf(ERROR, "Couldn't get device physical address\n");
      goto fail;
    }
  }

  {
    rndis_query_complete* mac_query_cmplt = static_cast<rndis_query_complete*>(buf);
    if (!command_succeeded(buf, RNDIS_QUERY_CMPLT,
                           sizeof(*mac_query_cmplt) + mac_query_cmplt->info_buffer_length)) {
      zxlogf(ERROR, "rndishost MAC query failed.\n");
      status = ZX_ERR_IO;
      goto fail;
    }
    uint8_t* mac_addr = static_cast<uint8_t*>(buf) + 8 + mac_query_cmplt->info_buffer_offset;
    memcpy(mac_addr_, mac_addr, sizeof(mac_addr_));
    zxlogf(INFO, "rndishost MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n", mac_addr_[0],
           mac_addr_[1], mac_addr_[2], mac_addr_[3], mac_addr_[4], mac_addr_[5]);
  }
  {
    // Enable data transfers
    memset(buf, 0, RNDIS_BUFFER_SIZE);
    rndis_set* set = static_cast<rndis_set*>(buf);
    set->msg_type = RNDIS_SET_MSG;
    set->msg_length = sizeof(rndis_set) + 4;  // 4 bytes for the filter
    set->oid = OID_GEN_CURRENT_PACKET_FILTER;
    set->info_buffer_length = 4;
    // Offset should begin at oid, so subtract 8 bytes for msg_type and msg_length.
    set->info_buffer_offset = sizeof(rndis_set) - 8;
    uint8_t* filter = static_cast<uint8_t*>(buf) + sizeof(rndis_set);
    *filter = RNDIS_PACKET_TYPE_DIRECTED | RNDIS_PACKET_TYPE_BROADCAST |
              RNDIS_PACKET_TYPE_ALL_MULTICAST | RNDIS_PACKET_TYPE_PROMISCUOUS;
    status = Command(buf);
    if (status < 0) {
      zxlogf(ERROR, "Couldn't set the packet filter.\n");
      goto fail;
    }

    if (!command_succeeded(buf, RNDIS_SET_CMPLT, sizeof(rndis_set_complete))) {
      zxlogf(ERROR, "rndishost set filter failed.\n");
      status = ZX_ERR_IO;
      goto fail;
    }
  }

  // Queue read requests
  {
    fbl::AutoLock lock(&mutex_);
    usb_request_t* txn;
    usb_request_complete_t complete = {
        .callback = [](void* arg, usb_request_t* request) -> void {
          static_cast<RndisHost*>(arg)->ReadComplete(request);
        },
        .ctx = this,
    };
    while ((txn = usb_req_list_remove_head(&free_read_reqs_, parent_req_size_)) != nullptr) {
      usb_request_queue(&usb_, txn, &complete);
    }
  }

  free(buf);

  DdkMakeVisible();
  return ZX_OK;

fail:
  free(buf);
  DdkAsyncRemove();
  return status;
}

zx_status_t RndisHost::AddDevice() {
  zx_status_t status = ZX_OK;

  uint64_t req_size = parent_req_size_ + sizeof(usb_req_internal_t);

  for (int i = 0; i < READ_REQ_COUNT; i++) {
    usb_request_t* req;
    status = usb_request_alloc(&req, RNDIS_BUFFER_SIZE, bulk_in_addr_, req_size);
    if (status != ZX_OK) {
      return status;
    }
    status = usb_req_list_add_head(&free_read_reqs_, req, parent_req_size_);
    ZX_DEBUG_ASSERT(status == ZX_OK);
  }
  for (int i = 0; i < WRITE_REQ_COUNT; i++) {
    usb_request_t* req;
    // TODO: Allocate based on mtu.
    status = usb_request_alloc(&req, RNDIS_BUFFER_SIZE, bulk_out_addr_, req_size);
    if (status != ZX_OK) {
      return status;
    }
    status = usb_req_list_add_head(&free_write_reqs_, req, parent_req_size_);
    ZX_DEBUG_ASSERT(status == ZX_OK);
  }

  fbl::AutoLock lock(&mutex_);
  status = DdkAdd("rndishost", DEVICE_ADD_INVISIBLE, nullptr, 0, ZX_PROTOCOL_ETHERNET_IMPL);
  if (status != ZX_OK) {
    lock.release();
    zxlogf(ERROR, "rndishost: failed to create device: %d\n", status);
    return status;
  }

  thread_started_ = true;
  int ret = thrd_create_with_name(
      &thread_, [](void* arg) -> int { return static_cast<RndisHost*>(arg)->StartThread(); }, this,
      "rndishost_start_thread");
  if (ret != thrd_success) {
    thread_started_ = false;
    lock.release();
    DdkAsyncRemove();
    return ZX_ERR_NO_RESOURCES;
  }

  return ZX_OK;
}

static zx_status_t rndishost_bind(void* ctx, zx_device_t* parent) {
  usb_protocol_t usb;
  zx_status_t status = device_get_protocol(parent, ZX_PROTOCOL_USB, &usb);
  if (status != ZX_OK) {
    return status;
  }

  // Find our endpoints.
  usb_desc_iter_t iter;
  status = usb_desc_iter_init(&usb, &iter);
  if (status < 0) {
    return status;
  }

  // We should have two interfaces: the CDC classified interface the bulk in
  // and out endpoints, and the RNDIS interface for control. The RNDIS
  // interface will be classified as USB_CLASS_WIRELESS when the device is
  // used for tethering.
  // TODO: Figure out how to handle other RNDIS use cases.
  usb_interface_descriptor_t* intf = usb_desc_iter_next_interface(&iter, false);
  uint8_t bulk_in_addr = 0;
  uint8_t bulk_out_addr = 0;
  uint8_t intr_addr = 0;
  uint8_t control_intf = 0;
  while (intf) {
    if (intf->bInterfaceClass == USB_CLASS_WIRELESS) {
      control_intf = intf->bInterfaceNumber;
      if (intf->bNumEndpoints != 1) {
        usb_desc_iter_release(&iter);
        return ZX_ERR_NOT_SUPPORTED;
      }
      usb_endpoint_descriptor_t* endp = usb_desc_iter_next_endpoint(&iter);
      while (endp) {
        if (usb_ep_direction(endp) == USB_ENDPOINT_IN &&
            usb_ep_type(endp) == USB_ENDPOINT_INTERRUPT) {
          intr_addr = endp->bEndpointAddress;
        }
        endp = usb_desc_iter_next_endpoint(&iter);
      }
    } else if (intf->bInterfaceClass == USB_CLASS_CDC) {
      if (intf->bNumEndpoints != 2) {
        usb_desc_iter_release(&iter);
        return ZX_ERR_NOT_SUPPORTED;
      }
      usb_endpoint_descriptor_t* endp = usb_desc_iter_next_endpoint(&iter);
      while (endp) {
        if (usb_ep_direction(endp) == USB_ENDPOINT_OUT) {
          if (usb_ep_type(endp) == USB_ENDPOINT_BULK) {
            bulk_out_addr = endp->bEndpointAddress;
          }
        } else if (usb_ep_direction(endp) == USB_ENDPOINT_IN) {
          if (usb_ep_type(endp) == USB_ENDPOINT_BULK) {
            bulk_in_addr = endp->bEndpointAddress;
          }
        }
        endp = usb_desc_iter_next_endpoint(&iter);
      }
    } else {
      usb_desc_iter_release(&iter);
      return ZX_ERR_NOT_SUPPORTED;
    }
    intf = usb_desc_iter_next_interface(&iter, false);
  }
  usb_desc_iter_release(&iter);

  if (!bulk_in_addr || !bulk_out_addr || !intr_addr) {
    zxlogf(ERROR, "rndishost couldn't find endpoints\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<RndisHost>(&ac, parent, control_intf, bulk_in_addr,
                                                 bulk_out_addr, usb);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  status = dev->AddDevice();
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev, so we don't own it any more.
    dev.release();
  } else {
    zxlogf(ERROR, "rndishost_bind failed: %d\n", status);
  }
  return status;
}

static zx_driver_ops_t rndis_driver_ops = []() {
  zx_driver_ops_t ops{};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = rndishost_bind;
  return ops;
}();

// TODO: Make sure we can bind to all RNDIS use cases. USB_CLASS_WIRELESS only
// covers the tethered device case.
// clang-format off
ZIRCON_DRIVER_BEGIN(rndishost, rndis_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB),
    BI_ABORT_IF(NE, BIND_USB_CLASS, USB_CLASS_WIRELESS),
    BI_ABORT_IF(NE, BIND_USB_SUBCLASS, RNDIS_SUBCLASS),
    BI_MATCH_IF(EQ, BIND_USB_PROTOCOL, RNDIS_PROTOCOL),
ZIRCON_DRIVER_END(rndishost)
