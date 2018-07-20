// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/mgr/vsock_endpoint.h"

#include "garnet/bin/guest/mgr/vsock_server.h"
#include "lib/fxl/logging.h"

namespace guestmgr {

VsockEndpoint::VsockEndpoint(uint32_t cid) : cid_(cid) {}

VsockEndpoint::~VsockEndpoint() {
  if (vsock_server_ != nullptr) {
    vsock_server_->RemoveEndpoint(cid_);
    vsock_server_ = nullptr;
  }
}

void VsockEndpoint::Connect(uint32_t src_port, uint32_t cid, uint32_t port,
                            ConnectCallback callback) {
  FXL_DCHECK(vsock_server_);
  VsockEndpoint* endpoint = vsock_server_->FindEndpoint(cid);
  if (endpoint == nullptr) {
    callback(ZX_ERR_CONNECTION_REFUSED, zx::handle());
    return;
  }
  zx::socket h1, h2;
  zx_status_t status = zx::socket::create(ZX_SOCKET_STREAM, &h1, &h2);
  if (status != ZX_OK) {
    callback(ZX_ERR_CONNECTION_REFUSED, zx::handle());
    return;
  }
  endpoint->Accept(cid_, src_port, port, std::move(h1),
                   [cb = std::move(callback),
                    h = std::move(h2)](zx_status_t status) mutable {
                     cb(status, status == ZX_OK ? std::move(h) : zx::socket());
                   });
}

}  //  namespace guestmgr
