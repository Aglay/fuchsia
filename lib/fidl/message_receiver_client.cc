// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/fidl/message_receiver_client.h"

#include <utility>

namespace fuchsia {
namespace modular {

MessageReceiverClient::MessageReceiverClient(
    fuchsia::modular::MessageQueue* const mq,
    MessageReceiverClientCallback callback)
    : callback_(std::move(callback)), receiver_(this) {
  mq->RegisterReceiver(receiver_.NewBinding());
}

MessageReceiverClient::~MessageReceiverClient() = default;

void MessageReceiverClient::OnReceive(fidl::StringPtr message,
                                      OnReceiveCallback ack) {
  callback_(message, ack);
}

}  // namespace modular
}  // namespace fuchsia
