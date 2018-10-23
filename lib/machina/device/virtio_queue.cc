// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/machina/device/virtio_queue.h"

#include <lib/fxl/logging.h>
#include <virtio/virtio_ring.h>

namespace machina {

VirtioQueue::VirtioQueue() {
  FXL_CHECK(zx::event::create(0, &event_) == ZX_OK);
}

void VirtioQueue::Configure(uint16_t size, zx_gpaddr_t desc, zx_gpaddr_t avail,
                            zx_gpaddr_t used) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Configure the ring size.
  ring_.size = size;

  // Configure the descriptor table.
  const uintptr_t desc_size = ring_.size * sizeof(ring_.desc[0]);
  ring_.desc = phys_mem_->as<vring_desc>(desc, desc_size);

  // Configure the available ring.
  const uintptr_t avail_size =
      sizeof(*ring_.avail) + (ring_.size * sizeof(ring_.avail->ring[0]));
  ring_.avail = phys_mem_->as<vring_avail>(avail, avail_size);

  const uintptr_t used_event_addr = avail + avail_size;
  ring_.used_event = phys_mem_->as<uint16_t>(used_event_addr);

  // Configure the used ring.
  const uintptr_t used_size =
      sizeof(*ring_.used) + (ring_.size * sizeof(ring_.used->ring[0]));
  ring_.used = phys_mem_->as<vring_used>(used, used_size);

  const uintptr_t avail_event_addr = used + used_size;
  ring_.avail_event = phys_mem_->as<uint16_t>(avail_event_addr);
}

bool VirtioQueue::NextChain(VirtioChain* chain) {
  uint16_t head;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!HasAvailLocked()) {
      return false;
    }
    head = ring_.avail->ring[RingIndexLocked(ring_.index++)];
  }
  *chain = VirtioChain(this, head);
  return true;
}

zx_status_t VirtioQueue::NextAvailLocked(uint16_t* index) {
  if (!HasAvailLocked()) {
    return ZX_ERR_SHOULD_WAIT;
  }

  *index = ring_.avail->ring[RingIndexLocked(ring_.index++)];

  // If we have event indices enabled, update the avail-event to notify us
  // when we have sufficient descriptors available.
  if (use_event_index_ && ring_.avail_event) {
    *ring_.avail_event = ring_.index + avail_event_num_ - 1;
  }

  if (!HasAvailLocked()) {
    return event_.signal(SIGNAL_QUEUE_AVAIL, 0);
  }
  return ZX_OK;
}

bool VirtioQueue::HasAvailLocked() const {
  return ring_.avail->idx != ring_.index;
}

uint32_t VirtioQueue::RingIndexLocked(uint32_t index) const {
  return index % ring_.size;
}

zx_status_t VirtioQueue::Notify() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (HasAvailLocked()) {
    return event_.signal(0, SIGNAL_QUEUE_AVAIL);
  }
  return ZX_OK;
}

zx_status_t VirtioQueue::PollAsync(async_dispatcher_t* dispatcher,
                                   async::Wait* wait, PollFn handler) {
  wait->set_object(event_.get());
  wait->set_trigger(SIGNAL_QUEUE_AVAIL);
  wait->set_handler([this, handler = std::move(handler)](
                        async_dispatcher_t* dispatcher, async::Wait* wait,
                        zx_status_t status, const zx_packet_signal_t* signal) {
    InvokeAsyncHandler(dispatcher, wait, status, handler);
  });
  return wait->Begin(dispatcher);
}

void VirtioQueue::InvokeAsyncHandler(async_dispatcher_t* dispatcher,
                                     async::Wait* wait, zx_status_t status,
                                     const PollFn& handler) {
  if (status != ZX_OK) {
    return;
  }

  uint16_t head;
  uint32_t used = 0;
  status = NextAvail(&head);
  if (status == ZX_OK) {
    status = handler(this, head, &used);
    // Try to return the buffer to the queue, even if the handler has failed
    // so we don't leak the descriptor.
    zx_status_t return_status = Return(head, used);
    if (status == ZX_OK) {
      status = return_status;
    }
  }
  if (status == ZX_OK || status == ZX_ERR_SHOULD_WAIT) {
    wait->Begin(dispatcher);  // ignore errors
  }
}

zx_status_t VirtioQueue::ReadDesc(uint16_t desc_index, VirtioDescriptor* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& desc = ring_.desc[desc_index];

  const uint64_t end = desc.addr + desc.len;
  if (end < desc.addr || end > phys_mem_->size()) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  out->addr = phys_mem_->as<void>(desc.addr, desc.len);
  out->len = desc.len;
  out->next = desc.next;
  out->has_next = desc.flags & VRING_DESC_F_NEXT;
  out->writable = desc.flags & VRING_DESC_F_WRITE;
  return ZX_OK;
}

zx_status_t VirtioQueue::Return(uint16_t index, uint32_t len, uint8_t actions) {
  bool needs_interrupt = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    volatile struct vring_used_elem* used =
        &ring_.used->ring[RingIndexLocked(ring_.used->idx)];

    used->id = index;
    used->len = len;
    ring_.used->idx++;

    // Virtio 1.0 Section 2.4.7.2: Virtqueue Interrupt Suppression
    if (!use_event_index_) {
      // If the VIRTIO_F_EVENT_IDX feature bit is not negotiated:
      //  - The device MUST ignore the used_event value.
      //  - After the device writes a descriptor index into the used ring:
      //    - If flags is 1, the device SHOULD NOT send an interrupt.
      //    - If flags is 0, the device MUST send an interrupt.
      needs_interrupt = ring_.used->flags == 0;
    } else if (ring_.used_event) {
      // Otherwise, if the VIRTIO_F_EVENT_IDX feature bit is negotiated:
      //
      //  - The device MUST ignore the lower bit of flags.
      //  - After the device writes a descriptor index into the used ring:
      //    - If the idx field in the used ring (which determined where that
      //      descriptor index was placed) was equal to used_event, the device
      //      MUST send an interrupt.
      //    - Otherwise the device SHOULD NOT send an interrupt.
      needs_interrupt = ring_.used->idx == (*ring_.used_event + 1);
    }
  }

  if (needs_interrupt) {
    return interrupt_(actions);
  }
  return ZX_OK;
}

VirtioChain::VirtioChain(VirtioQueue* queue, uint16_t head)
    : queue_(queue), head_(head), next_(head), has_next_(true) {}

bool VirtioChain::IsValid() const { return queue_ != nullptr; }

bool VirtioChain::HasDescriptor() const { return has_next_; }

bool VirtioChain::NextDescriptor(VirtioDescriptor* desc) {
  if (!HasDescriptor()) {
    return false;
  }
  zx_status_t status = queue_->ReadDesc(next_, desc);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to read queue " << status;
    return false;
  }
  next_ = desc->next;
  has_next_ = desc->has_next;
  return true;
}

uint32_t* VirtioChain::Used() { return &used_; }

void VirtioChain::Return(uint8_t actions) {
  zx_status_t status = queue_->Return(head_, used_, actions);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to return descriptor chain to queue " << status;
  }
  has_next_ = false;
}

}  // namespace machina
