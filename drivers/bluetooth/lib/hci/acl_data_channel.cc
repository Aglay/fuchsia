// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acl_data_channel.h"

#include <endian.h>
#include <magenta/status.h>

#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/string_printf.h"

#include "connection.h"
#include "transport.h"

namespace bluetooth {
namespace hci {

ACLDataChannel::ACLDataChannel(Transport* transport, mx::channel hci_acl_channel,
                               const ConnectionLookupCallback& conn_lookup_cb,
                               const DataReceivedCallback& rx_callback,
                               ftl::RefPtr<ftl::TaskRunner> rx_task_runner)
    : transport_(transport),
      channel_(std::move(hci_acl_channel)),
      conn_lookup_cb_(conn_lookup_cb),
      is_initialized_(false),
      event_handler_id_(0u),
      io_handler_key_(0u),
      rx_callback_(rx_callback),
      rx_task_runner_(rx_task_runner),
      max_data_len_(0u),
      le_max_data_len_(0u),
      max_num_packets_(0u),
      le_max_num_packets_(0u),
      num_sent_packets_(0u),
      le_num_sent_packets_(0u) {
  FTL_DCHECK(transport_), FTL_DCHECK(channel_.is_valid());
  FTL_DCHECK(conn_lookup_cb_);
  FTL_DCHECK(rx_callback);
  FTL_DCHECK(rx_task_runner_);
}

ACLDataChannel::~ACLDataChannel() {
  ShutDown();
}

void ACLDataChannel::Initialize(size_t max_data_len, size_t le_max_data_len, size_t max_num_packets,
                                size_t le_max_num_packets) {
  FTL_DCHECK(!is_initialized_);
  FTL_DCHECK(max_data_len);
  FTL_DCHECK(max_num_packets);

  max_data_len_ = max_data_len;
  le_max_data_len_ = le_max_data_len;
  max_num_packets_ = max_num_packets;
  le_max_num_packets_ = le_max_num_packets;

  // We make sure that this method blocks until the I/O handler registration task has run.
  std::mutex init_mutex;
  std::condition_variable init_cv;
  bool ready = false;

  io_task_runner_ = transport_->io_task_runner();
  io_task_runner_->PostTask([&] {
    // TODO(armansito): We'll need to pay attention to MX_CHANNEL_WRITABLE as well or perhaps not if
    // we switch to mx fifo.
    io_handler_key_ =
        mtl::MessageLoop::GetCurrent()->AddHandler(this, channel_.get(), MX_CHANNEL_READABLE);
    FTL_LOG(INFO) << "hci: ACLDataChannel: I/O handler registered";
    {
      std::lock_guard<std::mutex> lock(init_mutex);
      ready = true;
    }
    init_cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(init_mutex);
  init_cv.wait(lock, [&ready] { return ready; });

  event_handler_id_ = transport_->command_channel()->AddEventHandler(
      kNumberOfCompletedPacketsEventCode,
      std::bind(&ACLDataChannel::NumberOfCompletedPacketsCallback, this, std::placeholders::_1),
      io_task_runner_);
  FTL_DCHECK(event_handler_id_);

  is_initialized_ = true;

  FTL_LOG(INFO) << "hci: ACLDataChannel: initialized";
}

void ACLDataChannel::ShutDown() {
  if (!is_initialized_) return;

  FTL_LOG(INFO) << "hci: ACLDataChannel: shutting down";

  io_task_runner_->PostTask([handler_key = io_handler_key_] {
    FTL_DCHECK(mtl::MessageLoop::GetCurrent());
    FTL_LOG(INFO) << "hci: ACLDataChannel Removing I/O handler";
    mtl::MessageLoop::GetCurrent()->RemoveHandler(handler_key);
  });

  transport_->command_channel()->RemoveEventHandler(event_handler_id_);

  is_initialized_ = false;

  send_queue_ = std::queue<QueuedDataPacket>();
  io_task_runner_ = nullptr;
  io_handler_key_ = 0u;
  event_handler_id_ = 0u;
  rx_callback_ = DataReceivedCallback();
  rx_task_runner_ = nullptr;
}

size_t ACLDataChannel::GetMaxDataLength() const {
  return max_data_len_;
}

size_t ACLDataChannel::GetLEMaxDataLength() const {
  return le_max_data_len_ ? le_max_data_len_ : max_data_len_;
}

size_t ACLDataChannel::GetMaxNumberOfPackets() const {
  return max_num_packets_;
}

size_t ACLDataChannel::GetLEMaxNumberOfPackets() const {
  return le_max_num_packets_ ? le_max_num_packets_ : max_num_packets_;
}

bool ACLDataChannel::SendPacket(common::DynamicByteBuffer data_packet) {
  if (!is_initialized_) {
    FTL_VLOG(1) << "hci: ACLDataChannel: Cannot send packets while uninitialized";
    return false;
  }

  // Use ACLDataRxPacket to since we want a data packet "reader" not a "writer".
  ACLDataRxPacket acl_packet(&data_packet);
  auto conn = conn_lookup_cb_(acl_packet.GetConnectionHandle());
  if (!conn) {
    FTL_VLOG(1) << ftl::StringPrintf(
        "hci: ACLDataChannel: cannot send packet for unknown connection: 0x%04x",
        acl_packet.GetConnectionHandle());
    return false;
  }

  size_t mtu =
      (conn->type() == Connection::LinkType::kLE) ? GetLEMaxDataLength() : GetMaxDataLength();
  if (acl_packet.GetPayloadSize() > mtu) {
    FTL_LOG(ERROR) << "ACL data packet too large!";
    return false;
  }

  QueuedDataPacket queued_packet;
  queued_packet.bytes = std::move(data_packet);

  // We currently only support LE. We don't do anything fancy wrt buffer management.
  FTL_DCHECK(conn->type() == Connection::LinkType::kLE);

  std::lock_guard<std::mutex> lock(send_mutex_);
  send_queue_.push(std::move(queued_packet));
  io_task_runner_->PostTask(std::bind(&ACLDataChannel::TrySendNextQueuedPackets, this));

  return true;
}

void ACLDataChannel::NumberOfCompletedPacketsCallback(const EventPacket& event) {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(event.event_code() == kNumberOfCompletedPacketsEventCode);

  auto payload = event.GetPayload<NumberOfCompletedPacketsEventParams>();
  size_t total_comp_packets = 0;
  size_t le_total_comp_packets = 0;

  for (uint8_t i = 0; i < payload->number_of_handles; ++i) {
    const NumberOfCompletedPacketsEventData* data = payload->data + i;

    // TODO(armansito): This could be racy, i.e. the connection could be removed before we had a
    // chance to process this event. While the HCI guarantees that this event won't be received for
    // a connection handle after sending the corresponding disconnection event, we must take care to
    // process these events in the correct order.
    auto conn = conn_lookup_cb_(le16toh(data->connection_handle));
    FTL_DCHECK(conn);

    // TODO(armansito): This method should perform some sort of priority management so that each
    // connection handle gets to send its share of data based on a priority scheme. Right now we
    // send things on a FIFO basis.
    if (conn->type() == Connection::LinkType::kLE) {
      le_total_comp_packets += le16toh(data->hc_num_of_completed_packets);
    } else {
      total_comp_packets += le16toh(data->hc_num_of_completed_packets);
    }
  }

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    DecrementTotalNumPacketsLocked(total_comp_packets);
    DecrementLETotalNumPacketsLocked(le_total_comp_packets);
  }

  TrySendNextQueuedPackets();
}

void ACLDataChannel::TrySendNextQueuedPackets() {
  if (!is_initialized_) return;

  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  // TODO(armansito): We need to implement a proper packet scheduling algorithm here. Since this
  // can be expensive, it will likely make sense to do ACL data I/O on a dedicated thread instead
  // of using one shared thread for all HCI I/O (maybe?). The important things that need to
  // happen here:
  //
  //   1. Consuming packets from separate buffers for each LL handle;
  //   2. Avoiding latency per LL-connection by scheduling packets based on a priority scheme;
  //   3. Correct controller buffer management for LE and BR/EDR
  //
  // For now, we assume LE links only and process packets using a FIFO approach.

  std::queue<QueuedDataPacket> to_send;

  {
    std::lock_guard<std::mutex> lock(send_mutex_);

    if (MaxLENumPacketsReachedLocked()) return;

    size_t avail_packets = GetNumFreeLEPacketsLocked();

    while (!send_queue_.empty() && avail_packets) {
      to_send.push(std::move(send_queue_.front()));
      send_queue_.pop();
      --avail_packets;
    }
  }

  if (to_send.empty()) return;

  size_t num_packets_sent = 0;
  while (!to_send.empty()) {
    auto packet = std::move(to_send.front());
    to_send.pop();

    mx_status_t status =
        channel_.write(0, packet.bytes.GetData(), packet.bytes.GetSize(), nullptr, 0);
    if (status < 0) {
      // TODO(armansito): We'll almost certainly hit this case if the channel's buffer gets filled,
      // so we need to watch for MX_CHANNEL_WRITABLE.
      FTL_LOG(ERROR) << "hci: ACLDataChannel: Failed to send data packet to HCI driver ("
                     << mx_status_get_string(status) << ") - dropping packet";
      continue;
    }

    num_packets_sent++;
  }

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    IncrementLETotalNumPacketsLocked(num_packets_sent);
  }
}

size_t ACLDataChannel::GetNumFreeBREDRPacketsLocked() const {
  FTL_DCHECK(max_num_packets_ >= num_sent_packets_);
  return max_num_packets_ - num_sent_packets_;
}

size_t ACLDataChannel::GetNumFreeLEPacketsLocked() const {
  if (!le_max_num_packets_) return GetNumFreeBREDRPacketsLocked();

  FTL_DCHECK(le_max_num_packets_ >= le_num_sent_packets_);
  return le_max_num_packets_ - le_num_sent_packets_;
}

void ACLDataChannel::DecrementTotalNumPacketsLocked(size_t count) {
  FTL_DCHECK(num_sent_packets_ >= count);
  num_sent_packets_ -= count;
}

void ACLDataChannel::DecrementLETotalNumPacketsLocked(size_t count) {
  if (!le_max_num_packets_) {
    DecrementTotalNumPacketsLocked(count);
    return;
  }

  FTL_DCHECK(le_num_sent_packets_ >= count);
  le_num_sent_packets_ -= count;
}

void ACLDataChannel::IncrementTotalNumPacketsLocked(size_t count) {
  FTL_DCHECK(num_sent_packets_ + count <= max_num_packets_);
  num_sent_packets_ += count;
}

void ACLDataChannel::IncrementLETotalNumPacketsLocked(size_t count) {
  if (!le_max_num_packets_) {
    IncrementTotalNumPacketsLocked(count);
    return;
  }

  FTL_DCHECK(le_num_sent_packets_ + count <= le_max_num_packets_);
  le_num_sent_packets_ += count;
}

bool ACLDataChannel::MaxNumPacketsReachedLocked() const {
  return num_sent_packets_ == max_num_packets_;
}

bool ACLDataChannel::MaxLENumPacketsReachedLocked() const {
  if (!le_max_num_packets_) return MaxNumPacketsReachedLocked();
  return le_num_sent_packets_ == le_max_num_packets_;
}

void ACLDataChannel::OnHandleReady(mx_handle_t handle, mx_signals_t pending) {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(handle == channel_.get());
  FTL_DCHECK(pending & MX_CHANNEL_READABLE);

  uint32_t read_size;
  mx_status_t status = channel_.read(0u, rx_buffer_.GetMutableData(), rx_buffer_.GetSize(),
                                     &read_size, nullptr, 0, nullptr);
  if (status < 0) {
    FTL_VLOG(1) << "hci: ACLDataChannel: Failed to read RX bytes: " << mx_status_get_string(status);
    // Clear the handler so that we stop receiving events from it.
    mtl::MessageLoop::GetCurrent()->RemoveHandler(io_handler_key_);
    return;
  }

  if (read_size < sizeof(ACLDataHeader)) {
    FTL_LOG(ERROR) << "hci: ACLDataChannel: Malformed data packet - "
                   << "expected at least " << sizeof(ACLDataHeader) << " bytes, "
                   << "got " << read_size;
    return;
  }

  const size_t rx_payload_size = read_size - sizeof(ACLDataHeader);
  ACLDataRxPacket packet(&rx_buffer_);
  if (packet.GetPayloadSize() != rx_payload_size) {
    FTL_LOG(ERROR) << "hci: ACLDataChannel: Malformed packet - "
                   << "payload size from header (" << packet.GetPayloadSize() << ")"
                   << " does not match received payload size: " << rx_payload_size;
    return;
  }

  // TODO(armansito): We are copying the data here. Need more efficient buffer management.
  common::DynamicByteBuffer buffer(packet.size());
  memcpy(buffer.GetMutableData(), packet.buffer()->GetData(), packet.size());
  rx_task_runner_->PostTask(
      ftl::MakeCopyable([ buffer = std::move(buffer), callback = rx_callback_ ]() mutable {
        callback(std::move(buffer));
      }));
}

void ACLDataChannel::OnHandleError(mx_handle_t handle, mx_status_t error) {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(handle == channel_.get());

  FTL_VLOG(1) << "hci: ACLDataChannel: channel error: " << mx_status_get_string(error);

  // Clear the handler so that we stop receiving events from it.
  mtl::MessageLoop::GetCurrent()->RemoveHandler(io_handler_key_);
}

}  // namespace hci
}  // namespace bluetooth
