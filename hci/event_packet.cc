// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "event_packet.h"

#include "lib/ftl/logging.h"

namespace bluetooth {
namespace hci {

EventPacket::EventPacket(EventCode event_code,
                         common::MutableByteBuffer* buffer,
                         size_t payload_size)
    : common::Packet<EventHeader>(buffer, payload_size) {
  FTL_DCHECK(GetPayloadSize() <= kMaxEventPacketPayloadSize);
  GetMutableHeader()->event_code = event_code;
}

void EventPacket::EncodeHeader() {
  FTL_DCHECK(GetPayloadSize() <= kMaxEventPacketPayloadSize);
  GetMutableHeader()->parameter_total_size =
      static_cast<uint8_t>(GetPayloadSize());
}

}  // namespace hci
}  // namespace bluetooth
