// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <atomic>
#include <mutex>

#include <magenta/compiler.h>

#include "apps/bluetooth/lib/l2cap/sdu.h"
#include "lib/ftl/functional/closure.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/memory/ref_ptr.h"
#include "lib/ftl/synchronization/thread_checker.h"
#include "lib/ftl/tasks/task_runner.h"

namespace bluetooth {
namespace l2cap {

// Represents a L2CAP channel. Each instance is owned by a service implementation that operates on
// the corresponding channel. Instances are created by and associated with a LogicalLink.
//
// A Channel can operate in one of 6 L2CAP Modes of Operation (see Core Spec v5.0, Vol 3, Part A,
// Section 2.4). Only Basic Mode is currently supported.
//
// USAGE:
//
// Channel is an abstract base class. There are two concrete implementations:
//
//   * internal::ChannelImpl (defined below) which implements a real L2CAP channel. Instances are
//     obtained from ChannelManager and tied to internal::LogicalLink instances.
//
//   * FakeChannel, which can be used for unit testing service-layer entities that operate on one or
//     more L2CAP channel(s).
//     TODO(armansito): Introduce FakeChannel later.
//
// THREAD-SAFETY:
//
// This class is thread-safe with the following caveats:
//
//   * Creation and deletion must always happen on the creation thread of the L2CAP ChannelManager.
//
//   * RxCallback will be accessed and frequently copied on the HCI I/O thread. Callers should take
//     care while managing the life time of objects that are referenced by the callback.
class Channel {
 public:
  virtual ~Channel() = default;

  ChannelId id() const { return id_; }

  // Sends the given payload over this channel. |payload| corresponds to the information payload of
  // a basic L2CAP frame.
  virtual void SendBasicFrame(const common::ByteBuffer& payload) = 0;

  // Callback invoked when this channel has been closed without an explicit request from the owner
  // of this instance. For example, this can happen when the remote end closes a dynamically
  // configured channel or when the underlying logical link is terminated through other means.
  //
  // This callback is always run on this Channel's creation thread.
  using ClosedCallback = ftl::Closure;
  void set_channel_closed_callback(const ClosedCallback& callback) { closed_cb_ = callback; }

  // Callback invoked when a new SDU is received on this channel. |rx_cb| will always be posted on
  // |rx_task_runner|. If a non-empty |rx_cb| is provided, then the value of |rx_task_runner| must
  // not be nullptr. If |rx_cb| is empty (e.g. to clear the rx handler), then |rx_task_runner| must
  // be nullptr.
  using RxCallback = std::function<void(const SDU& sdu)>;
  void SetRxHandler(const RxCallback& rx_cb, ftl::RefPtr<ftl::TaskRunner> rx_task_runner);

 protected:
  explicit Channel(ChannelId id);

  const ClosedCallback& closed_callback() const { return closed_cb_; }

  bool IsCreationThreadCurrent() const { return thread_checker_.IsCreationThreadCurrent(); }

 private:
  ChannelId id_;
  ClosedCallback closed_cb_;

  std::mutex mtx_;
  RxCallback rx_cb_ __TA_GUARDED(mtx_);
  ftl::RefPtr<ftl::TaskRunner> rx_task_runner_ __TA_GUARDED(mtx_);

  ftl::ThreadChecker thread_checker_;

  FTL_DISALLOW_COPY_AND_ASSIGN(Channel);
};

namespace internal {

class LogicalLink;

// Channel implementation used in production.
class ChannelImpl : public Channel {
 public:
  ~ChannelImpl() override;

  void SendBasicFrame(const common::ByteBuffer& payload) override;

 private:
  friend class internal::LogicalLink;

  // Only a LogicalLink can construct a ChannelImpl.
  ChannelImpl(ChannelId id, internal::LogicalLink* link);

  // Called by |link_| to notify us when the channel can no longer process data. This MUST NOT call
  // any locking methods of |link_| as that WILL cause a deadlock.
  void OnLinkClosed();

  // The LogicalLink that this channel is associated with. A channel is always created by a
  // LogicalLink.
  //
  // |link_| is guaranteed to be valid as long as the link is active. When a LogicalLink is torn
  // down, it will notify all of its associated channels by calling OnLinkClosed() which sets
  // |link_| to nullptr.
  std::mutex mtx_;
  internal::LogicalLink* link_ __TA_GUARDED(mtx_);  // weak

  FTL_DISALLOW_COPY_AND_ASSIGN(ChannelImpl);
};

}  // namespace internal
}  // namespace l2cap
}  // namespace bluetooth
