// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "apps/bluetooth/lib/hci/acl_data_channel.h"
#include "apps/bluetooth/lib/hci/command_channel.h"
#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/memory/ref_counted.h"
#include "lib/ftl/memory/ref_ptr.h"
#include "lib/ftl/memory/weak_ptr.h"
#include "lib/ftl/synchronization/thread_checker.h"
#include "lib/ftl/tasks/task_runner.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/tasks/message_loop_handler.h"

namespace bluetooth {
namespace hci {

// Represents the HCI transport layer. This object owns the HCI command, ACL,
// and SCO channels and provides the necessary control-flow mechanisms to send
// and receive HCI packets from the underlying Bluetooth controller.
//
// Transport expects to be initialized and shut down (via Initialize() and ShutDown()) on the thread
// it was created on. Initialize()/ShutDown() are NOT thread-safe.
//
// TODO(armansito): This class is ref-counted to prevent potential use-after-free errors though
// vending weak ptrs would have been more suitable since this class is intended to be uniquely owned
// by its creator. ftl::WeakPtr is not thread-safe which is why we use ftl::RefCountedThreadSafe.
// Consider making ftl::WeakPtr thread-safe.
class Transport final : public ::mtl::MessageLoopHandler,
                        public ftl::RefCountedThreadSafe<Transport> {
 public:
  // |device_fd| must be a valid file descriptor to a Bluetooth HCI device.
  static ftl::RefPtr<Transport> Create(ftl::UniqueFD device_fd);

  // Default constructor used only from tests.
  static ftl::RefPtr<Transport> Create();

  // Initializes the HCI command channel, starts the I/O event loop, and kicks off a new I/O thread
  // for transactions with the HCI driver. The ACLDataChannel will be left uninitialized. The
  // ACLDataChannel must be initialized after available data buffer information has been obtained
  // from the controller (via HCI_Read_Buffer_Size and HCI_LE_Read_Buffer_Size).
  //
  // This method is NOT thread-safe! Care must be taken such that the public methods of this
  // class and those of the individual channel classes are not called in a manner that would race
  // with the execution of Initialize().
  bool Initialize();

  // Initializes the ACL data channel with the given parameters. Returns false if an error occurs
  // during initialization. Initialize() must have been called successfully prior to calling this
  // method.
  bool InitializeACLDataChannel(size_t max_data_len, size_t le_max_data_len, size_t max_num_packets,
                                size_t le_max_num_packets,
                                const ACLDataChannel::ConnectionLookupCallback& conn_lookup_cb,
                                const ACLDataChannel::DataReceivedCallback& rx_callback,
                                ftl::RefPtr<ftl::TaskRunner> rx_task_runner);

  // Cleans up all transport channels, stops the I/O event loop, and joins the I/O thread. Once a
  // Transport has been shut down, it cannot be re-initialized.
  //
  // NOTE: Care must be taken such that this method is not called from a thread that would race with
  // a call to Initialize(). ShutDown() is not thread-safe; Initialize(),
  // InitializeACLDataChannel(), and ShutDown() MUST be called on the same thread.
  void ShutDown();

  // Returns true if this Transport has been fully initialized and running.
  bool IsInitialized() const;

  // Returns a pointer to the HCI command and event flow control handler.
  CommandChannel* command_channel() const { return command_channel_.get(); }

  // Returns a pointer to the HCI ACL data flow control handler.
  ACLDataChannel* acl_data_channel() const { return acl_data_channel_.get(); }

  // Returns the I/O thread task runner. If this is called when this Transport instance is not
  // initialized, the return value will be nullptr.
  ftl::RefPtr<ftl::TaskRunner> io_task_runner() const { return io_task_runner_; }

  // Set a callback that should be invoked when any one of the underlying channels gets closed
  // for any reason (e.g. the HCI device has disappeared) and the task runner on which the
  // callback should be posted.
  //
  // When this callback is called the channels will be in an invalid state and packet processing
  // is no longer guaranteed to work. It is the responsibility of the callback implementation to
  // clean up this Transport instance by calling ShutDown() and/or deleting it.
  void SetTransportClosedCallback(const ftl::Closure& callback,
                                  ftl::RefPtr<ftl::TaskRunner> task_runner);

  // Initialize function called from tests. |cmd_channel| cannot be nullptr. |acl_data_channel| can
  // be nullptr if it is not needed by a test.
  //
  // This simply spawns the I/O thread and takes ownership of the provided channels. The channels
  // themselves should be initialized explicitly after calling this function.
  void InitializeForTesting(std::unique_ptr<CommandChannel> cmd_channel,
                            std::unique_ptr<ACLDataChannel> acl_data_channel);

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(Transport);

  explicit Transport(ftl::UniqueFD device_fd);
  Transport();
  ~Transport() override;

  // ::mtl::MessageLoopHandler overrides:
  void OnHandleReady(mx_handle_t handle, mx_signals_t pending) override;
  void OnHandleError(mx_handle_t handle, mx_status_t error) override;

  // Notifies the closed callback.
  void NotifyClosedCallback();

  // Used to assert that certain public functions are only called on the creation thread.
  ftl::ThreadChecker thread_checker_;

  // The Bluetooth HCI device file descriptor.
  ftl::UniqueFD device_fd_;

  // The state of the initialization sequence.
  std::atomic_bool is_initialized_;

  // The thread that performs all HCI I/O operations.
  std::thread io_thread_;

  // The HandlerKey returned from mtl::MessageLoop::AddHandler
  mtl::MessageLoop::HandlerKey cmd_channel_handler_key_;
  mtl::MessageLoop::HandlerKey acl_channel_handler_key_;

  // The task runner used for posting tasks on the HCI transport I/O thread.
  ftl::RefPtr<ftl::TaskRunner> io_task_runner_;

  // The ACL data flow control handler.
  std::unique_ptr<ACLDataChannel> acl_data_channel_;

  // The HCI command and event flow control handler.
  std::unique_ptr<CommandChannel> command_channel_;

  // Callback invoked when the transport is closed (due to a channel error) and its task runner.
  ftl::Closure closed_cb_;
  ftl::RefPtr<ftl::TaskRunner> closed_cb_task_runner_;

  FTL_DISALLOW_COPY_AND_ASSIGN(Transport);
};

}  // namespace hci
}  // namespace bluetooth
