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
#include "slab_allocators.h"
#include "transport.h"

namespace bluetooth {
namespace hci {

DataBufferInfo::DataBufferInfo(size_t max_data_length, size_t max_num_packets)
    : max_data_length_(max_data_length), max_num_packets_(max_num_packets) {
  FTL_DCHECK(max_data_length_);
  FTL_DCHECK(max_num_packets_);
}

DataBufferInfo::DataBufferInfo() : max_data_length_(0u), max_num_packets_(0u) {}

bool DataBufferInfo::operator==(const DataBufferInfo& other) const {
  return max_data_length_ == other.max_data_length_ && max_num_packets_ == other.max_num_packets_;
}

ACLDataChannel::ACLDataChannel(Transport* transport, mx::channel hci_acl_channel,
                               const ConnectionLookupCallback& conn_lookup_cb)
    : transport_(transport),
      channel_(std::move(hci_acl_channel)),
      conn_lookup_cb_(conn_lookup_cb),
      is_initialized_(false),
      event_handler_id_(0u),
      io_handler_key_(0u),
      num_sent_packets_(0u),
      le_num_sent_packets_(0u) {
  FTL_DCHECK(transport_), FTL_DCHECK(channel_.is_valid());
  FTL_DCHECK(conn_lookup_cb_);
}

ACLDataChannel::~ACLDataChannel() {
  ShutDown();
}

void ACLDataChannel::Initialize(const DataBufferInfo& bredr_buffer_info,
                                const DataBufferInfo& le_buffer_info) {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  FTL_DCHECK(!is_initialized_);
  FTL_DCHECK(bredr_buffer_info.IsAvailable() || le_buffer_info.IsAvailable());

  bredr_buffer_info_ = bredr_buffer_info;
  le_buffer_info_ = le_buffer_info;

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
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  if (!is_initialized_) return;

  FTL_LOG(INFO) << "hci: ACLDataChannel: shutting down";

  io_task_runner_->PostTask([handler_key = io_handler_key_] {
    FTL_DCHECK(mtl::MessageLoop::GetCurrent());
    FTL_LOG(INFO) << "hci: ACLDataChannel Removing I/O handler";
    mtl::MessageLoop::GetCurrent()->RemoveHandler(handler_key);
  });

  transport_->command_channel()->RemoveEventHandler(event_handler_id_);

  is_initialized_ = false;

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_ = std::queue<QueuedDataPacket>();
  }

  io_task_runner_ = nullptr;
  io_handler_key_ = 0u;
  event_handler_id_ = 0u;
  SetDataRxHandler(nullptr, nullptr);
}

void ACLDataChannel::SetDataRxHandler(const DataReceivedCallback& rx_callback,
                                      ftl::RefPtr<ftl::TaskRunner> rx_task_runner) {
  // Make sure that if |rx_callback| is null, so is |rx_task_runner|.
  FTL_DCHECK(!!rx_callback == !!rx_task_runner);

  std::lock_guard<std::mutex> lock(rx_mutex_);
  rx_callback_ = rx_callback;
  rx_task_runner_ = rx_task_runner;
}

bool ACLDataChannel::SendPacket(std::unique_ptr<ACLDataPacket> data_packet) {
  if (!is_initialized_) {
    FTL_VLOG(1) << "hci: ACLDataChannel: Cannot send packets while uninitialized";
    return false;
  }

  FTL_DCHECK(data_packet);

  auto conn = conn_lookup_cb_(data_packet->connection_handle());
  if (!conn) {
    FTL_VLOG(1) << ftl::StringPrintf(
        "hci: ACLDataChannel: cannot send packet for unknown connection: 0x%04x",
        data_packet->connection_handle());
    return false;
  }

  if (data_packet->view().payload_size() > GetBufferMTU(*conn)) {
    FTL_LOG(ERROR) << "ACL data packet too large!";
    return false;
  }

  QueuedDataPacket queued_packet;
  queued_packet.packet = std::move(data_packet);

  // We currently only support LE. We don't do anything fancy wrt buffer management.
  FTL_DCHECK(conn->type() == Connection::LinkType::kLE);

  std::lock_guard<std::mutex> lock(send_mutex_);
  send_queue_.push(std::move(queued_packet));
  io_task_runner_->PostTask(std::bind(&ACLDataChannel::TrySendNextQueuedPackets, this));

  return true;
}

const DataBufferInfo& ACLDataChannel::GetBufferInfo() const {
  return bredr_buffer_info_;
}

const DataBufferInfo& ACLDataChannel::GetLEBufferInfo() const {
  return le_buffer_info_.IsAvailable() ? le_buffer_info_ : bredr_buffer_info_;
}

size_t ACLDataChannel::GetBufferMTU(const Connection& connection) {
  if (connection.type() != Connection::LinkType::kLE) return bredr_buffer_info_.max_data_length();

  return GetLEBufferInfo().max_data_length();
}

void ACLDataChannel::NumberOfCompletedPacketsCallback(const EventPacket& event) {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(event.event_code() == kNumberOfCompletedPacketsEventCode);

  const auto& payload = event.view().payload<NumberOfCompletedPacketsEventParams>();
  size_t total_comp_packets = 0;
  size_t le_total_comp_packets = 0;

  for (uint8_t i = 0; i < payload.number_of_handles; ++i) {
    const NumberOfCompletedPacketsEventData* data = payload.data + i;

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
    const QueuedDataPacket& packet = to_send.front();

    auto packet_bytes = packet.packet->view().data();
    mx_status_t status = channel_.write(0, packet_bytes.data(), packet_bytes.size(), nullptr, 0);
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
  FTL_DCHECK(bredr_buffer_info_.max_num_packets() >= num_sent_packets_);
  return bredr_buffer_info_.max_num_packets() - num_sent_packets_;
}

size_t ACLDataChannel::GetNumFreeLEPacketsLocked() const {
  if (!le_buffer_info_.IsAvailable()) return GetNumFreeBREDRPacketsLocked();

  FTL_DCHECK(le_buffer_info_.max_num_packets() >= le_num_sent_packets_);
  return le_buffer_info_.max_num_packets() - le_num_sent_packets_;
}

void ACLDataChannel::DecrementTotalNumPacketsLocked(size_t count) {
  FTL_DCHECK(num_sent_packets_ >= count);
  num_sent_packets_ -= count;
}

void ACLDataChannel::DecrementLETotalNumPacketsLocked(size_t count) {
  if (!le_buffer_info_.IsAvailable()) {
    DecrementTotalNumPacketsLocked(count);
    return;
  }

  FTL_DCHECK(le_num_sent_packets_ >= count);
  le_num_sent_packets_ -= count;
}

void ACLDataChannel::IncrementTotalNumPacketsLocked(size_t count) {
  FTL_DCHECK(num_sent_packets_ + count <= bredr_buffer_info_.max_num_packets());
  num_sent_packets_ += count;
}

void ACLDataChannel::IncrementLETotalNumPacketsLocked(size_t count) {
  if (!le_buffer_info_.IsAvailable()) {
    IncrementTotalNumPacketsLocked(count);
    return;
  }

  FTL_DCHECK(le_num_sent_packets_ + count <= le_buffer_info_.max_num_packets());
  le_num_sent_packets_ += count;
}

bool ACLDataChannel::MaxNumPacketsReachedLocked() const {
  return num_sent_packets_ == bredr_buffer_info_.max_num_packets();
}

bool ACLDataChannel::MaxLENumPacketsReachedLocked() const {
  if (!le_buffer_info_.IsAvailable()) return MaxNumPacketsReachedLocked();
  return le_num_sent_packets_ == le_buffer_info_.max_num_packets();
}

void ACLDataChannel::OnHandleReady(mx_handle_t handle, mx_signals_t pending, uint64_t count) {
  if (!is_initialized_) return;

  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(handle == channel_.get());
  FTL_DCHECK(pending & MX_CHANNEL_READABLE);

  std::lock_guard<std::mutex> lock(rx_mutex_);
  if (!rx_callback_) return;

  // Allocate a buffer for the event. Since we don't know the size beforehand we allocate the
  // largest possible buffer.
  auto packet = ACLDataPacket::New(slab_allocators::kLargeACLDataPayloadSize);
  if (!packet) {
    FTL_LOG(ERROR) << "Failed to allocate buffer received ACL data packet!";
    return;
  }

  uint32_t read_size;
  auto packet_bytes = packet->mutable_view()->mutable_data();
  mx_status_t status = channel_.read(0u, packet_bytes.mutable_data(), packet_bytes.size(),
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
  const size_t size_from_header = packet->view().header().data_total_length;
  if (size_from_header != rx_payload_size) {
    FTL_LOG(ERROR) << "hci: ACLDataChannel: Malformed packet - "
                   << "payload size from header (" << size_from_header << ")"
                   << " does not match received payload size: " << rx_payload_size;
    return;
  }

  packet->InitializeFromBuffer();

  rx_task_runner_->PostTask(
      ftl::MakeCopyable([ packet = std::move(packet), callback = rx_callback_ ]() mutable {
        callback(std::move(packet));
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
