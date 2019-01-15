// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/vfs/cpp/connection.h>

#include <lib/vfs/cpp/node.h>

namespace vfs {

Connection::Connection(uint32_t flags) : flags_(flags) {}

Connection::~Connection() = default;

void Connection::Clone(Node* vn, uint32_t flags,
                       fidl::InterfaceRequest<fuchsia::io::Node> object,
                       async_dispatcher_t* dispatcher) {
  // TODO: Check flags.
  vn->Serve(flags, object.TakeChannel(), dispatcher);
}

void Connection::Close(Node* vn, fuchsia::io::Node::CloseCallback callback) {
  callback(vn->Close());
  vn->RemoveConnection(this);
  // |this| is destroyed at this point.
}

void Connection::Describe(Node* vn,
                          fuchsia::io::Node::DescribeCallback callback) {
  fuchsia::io::NodeInfo info{};
  vn->Describe(&info);
  callback(std::move(info));
}

void Connection::Sync(Node* vn, fuchsia::io::Node::SyncCallback callback) {
  // TODO: Check flags.
  callback(vn->Sync());
}

void Connection::GetAttr(Node* vn,
                         fuchsia::io::Node::GetAttrCallback callback) {
  // TODO: Check flags.
  fuchsia::io::NodeAttributes attributes{};
  zx_status_t status = vn->GetAttr(&attributes);
  callback(status, attributes);
}

void Connection::SetAttr(Node* vn, uint32_t flags,
                         fuchsia::io::NodeAttributes attributes,
                         fuchsia::io::Node::SetAttrCallback callback) {
  // TODO: Check flags.
  callback(vn->SetAttr(flags, attributes));
}

void Connection::Ioctl(Node* vn, uint32_t opcode, uint64_t max_out,
                       std::vector<zx::handle> handles, std::vector<uint8_t> in,
                       fuchsia::io::Node::IoctlCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED, std::vector<zx::handle>(),
           std::vector<uint8_t>());
}

}  // namespace vfs
