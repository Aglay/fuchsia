// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "transport.h"

#include <magenta/status.h>
#include <mx/channel.h>

#include "lib/ftl/logging.h"
#include "lib/mtl/threading/create_thread.h"

#include "device_wrapper.h"

namespace bluetooth {
namespace hci {

// static
ftl::RefPtr<Transport> Transport::Create(std::unique_ptr<DeviceWrapper> hci_device) {
  return AdoptRef(new Transport(std::move(hci_device)));
}

Transport::Transport(std::unique_ptr<DeviceWrapper> hci_device)
    : hci_device_(std::move(hci_device)),
      is_initialized_(false),
      cmd_channel_handler_key_(0u),
      acl_channel_handler_key_(0u) {
  FTL_DCHECK(hci_device_);
}

Transport::~Transport() {
  if (IsInitialized()) ShutDown();
}

bool Transport::Initialize() {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  FTL_DCHECK(hci_device_);
  FTL_DCHECK(!command_channel_);
  FTL_DCHECK(!acl_data_channel_);
  FTL_DCHECK(!IsInitialized());

  // Obtain command channel handle.
  mx::channel channel = hci_device_->GetCommandChannel();
  if (!channel.is_valid()) {
    FTL_LOG(ERROR) << "hci: Transport: Failed to obtain command channel handle";
    return false;
  }

  io_thread_ = mtl::CreateThread(&io_task_runner_, "hci-transport");

  // We watch for handle errors and closures to perform the necessary clean up.
  io_task_runner_->PostTask([ handle = channel.get(), this ] {
    cmd_channel_handler_key_ =
        mtl::MessageLoop::GetCurrent()->AddHandler(this, handle, MX_CHANNEL_PEER_CLOSED);
  });

  command_channel_ = std::make_unique<CommandChannel>(this, std::move(channel));
  command_channel_->Initialize();

  is_initialized_ = true;

  return true;
}

bool Transport::InitializeACLDataChannel(
    const DataBufferInfo& bredr_buffer_info, const DataBufferInfo& le_buffer_info,
    const ACLDataChannel::ConnectionLookupCallback& conn_lookup_cb) {
  FTL_DCHECK(hci_device_);
  FTL_DCHECK(IsInitialized());

  // Obtain ACL data channel handle.
  mx::channel channel = hci_device_->GetACLDataChannel();
  if (!channel.is_valid()) {
    FTL_LOG(ERROR) << "hci: Transport: Failed to obtain ACL data channel handle";
    return false;
  }

  // We watch for handle errors and closures to perform the necessary clean up.
  io_task_runner_->PostTask([ handle = channel.get(), this ] {
    acl_channel_handler_key_ =
        mtl::MessageLoop::GetCurrent()->AddHandler(this, handle, MX_CHANNEL_PEER_CLOSED);
  });

  acl_data_channel_ = std::make_unique<ACLDataChannel>(this, std::move(channel), conn_lookup_cb);
  acl_data_channel_->Initialize(bredr_buffer_info, le_buffer_info);

  return true;
}

void Transport::SetTransportClosedCallback(const ftl::Closure& callback,
                                           ftl::RefPtr<ftl::TaskRunner> task_runner) {
  FTL_DCHECK(callback);
  FTL_DCHECK(task_runner);
  FTL_DCHECK(!closed_cb_);
  FTL_DCHECK(!closed_cb_task_runner_);

  closed_cb_ = callback;
  closed_cb_task_runner_ = task_runner;
}

void Transport::ShutDown() {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  FTL_DCHECK(IsInitialized());

  FTL_LOG(INFO) << "hci: Transport: shutting down";

  if (acl_data_channel_) acl_data_channel_->ShutDown();
  if (command_channel_) command_channel_->ShutDown();

  io_task_runner_->PostTask(
      [ cmd_key = cmd_channel_handler_key_, acl_key = acl_channel_handler_key_ ] {
        FTL_DCHECK(mtl::MessageLoop::GetCurrent());
        mtl::MessageLoop::GetCurrent()->RemoveHandler(cmd_key);
        mtl::MessageLoop::GetCurrent()->RemoveHandler(acl_key);
        mtl::MessageLoop::GetCurrent()->QuitNow();
      });

  if (io_thread_.joinable()) io_thread_.join();

  // We avoid deallocating the channels here as they *could* still be accessed by other threads.
  // It's OK to clear |io_task_runner_| as the channels hold their own references to it.
  //
  // Once |io_thread_| joins above, |io_task_runner_| will be defunct. However, the channels are
  // allowed to keep posting tasks on it (which will never execute).

  io_task_runner_ = nullptr;

  is_initialized_ = false;

  FTL_LOG(INFO) << "hci: Transport I/O loop exited";
}

bool Transport::IsInitialized() const {
  return is_initialized_;
}

void Transport::OnHandleReady(mx_handle_t handle, mx_signals_t pending) {
  FTL_DCHECK(pending & MX_CHANNEL_PEER_CLOSED);
  NotifyClosedCallback();
}

void Transport::OnHandleError(mx_handle_t handle, mx_status_t error) {
  FTL_LOG(ERROR) << "hci: Transport: channel error: " << mx_status_get_string(error);
  NotifyClosedCallback();
}

void Transport::NotifyClosedCallback() {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  // Clear the handlers so that we stop receiving events.
  mtl::MessageLoop::GetCurrent()->RemoveHandler(cmd_channel_handler_key_);
  mtl::MessageLoop::GetCurrent()->RemoveHandler(acl_channel_handler_key_);

  FTL_LOG(INFO) << "hci: Transport: HCI channel(s) were closed";
  if (closed_cb_) closed_cb_task_runner_->PostTask(closed_cb_);
}

}  // namespace hci
}  // namespace bluetooth
