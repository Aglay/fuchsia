// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "lib/wlan/fidl/wlan_mlme.fidl.h"

namespace wlan {

// ServiceHeader is the method header that is prepended to method calls over the channel.
// This will be removed when FIDL2 is available.
struct ServiceHeader {
  uint64_t len;
  uint64_t txn_id;
  uint32_t flags;
  uint32_t ordinal;
  uint8_t payload[];
} __PACKED;

template <typename T, typename FidlStruct = ::f1dl::StructPtr<T>>
zx_status_t DeserializeServiceMsg(const Packet& packet, Method m, FidlStruct* out) {
    if (out == nullptr) return ZX_ERR_INVALID_ARGS;

    const uint8_t* p = packet.data();
    auto h = FromBytes<ServiceHeader>(p, packet.len());
    if (static_cast<Method>(h->ordinal) != m) return ZX_ERR_IO;

    *out = T::New();
    auto reqptr = reinterpret_cast<const void*>(h->payload);
    if (!(*out)->Deserialize(const_cast<void*>(reqptr), packet.len() - h->len)) {
        return ZX_ERR_IO;
    }
    return ZX_OK;
}

template <typename T> zx_status_t SerializeServiceMsg(Packet* packet, Method m, const T& msg) {
    size_t buf_len = sizeof(ServiceHeader) + msg->GetSerializedSize();
    auto header = FromBytes<ServiceHeader>(packet->mut_data(), buf_len);
    if (header == nullptr) { return ZX_ERR_BUFFER_TOO_SMALL; }
    header->len = sizeof(ServiceHeader);
    header->txn_id = 1;  // TODO(tkilbourn): txn ids
    header->flags = 0;
    header->ordinal = static_cast<uint32_t>(m);
    if (!msg->Serialize(header->payload, buf_len - sizeof(ServiceHeader))) { return ZX_ERR_IO; }
    return ZX_OK;
}

}  // namespace wlan
