// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "l2cap_socket_channel_relay.h"

// These functions are |inline|, |static|, and in an unnamed-namespace, to avoid
// violating the one-definition rule. See
// https://en.cppreference.com/w/cpp/language/definition and
// https://en.cppreference.com/w/cpp/language/inline
//
// It may be redundant to place the definitions in an unnamed namespace,
// and declare them static, since either option should give the functions
// internal linkage. However, the |inline| documentation cited above only
// explicitly mentions |static| as an example of non-external linkage.
namespace {
using btlib::l2cap::SDU;
static inline bool ValidateRxData(const SDU& sdu) { return sdu.is_valid(); }
static inline size_t GetRxDataLen(const SDU& sdu) { return sdu.length(); }
static inline bool InvokeWithRxData(
    fit::function<void(const btlib::common::ByteBuffer& data)> callback,
    const SDU& sdu) {
  return SDU::Reader(&sdu).ReadNext(sdu.length(), callback);
}
}  // namespace

#include "garnet/drivers/bluetooth/lib/data/socket_channel_relay.cc"

namespace btlib::data::internal {
template class SocketChannelRelay<l2cap::Channel, l2cap::SDU>;
}  // namespace btlib::data::internal
