// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_WLAN_COMMON_INCLUDE_WLAN_COMMON_FROM_BYTES_H_
#define GARNET_LIB_WLAN_COMMON_INCLUDE_WLAN_COMMON_FROM_BYTES_H_

#include <wlan/common/span.h>
#include <cstdint>

namespace wlan {
    template <typename T> T* FromBytes(uint8_t* buf, size_t len) {
        if (len < sizeof(T)) return nullptr;
        return reinterpret_cast<T*>(buf);
    }

    template <typename T> const T* FromBytes(const uint8_t* buf, size_t len) {
        if (len < sizeof(T)) return nullptr;
        return reinterpret_cast<const T*>(buf);
    }

    template <typename T> const T* FromBytes(Span<const uint8_t> bytes) {
        if (bytes.size() < sizeof(T)) return nullptr;
        return reinterpret_cast<const T*>(bytes.data());
    }
}

#endif // GARNET_LIB_WLAN_COMMON_INCLUDE_WLAN_COMMON_FROM_BYTES_H_
