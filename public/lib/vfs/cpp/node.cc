// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/vfs/cpp/node.h>

#include <algorithm>

#include <fuchsia/io/c/fidl.h>
#include <lib/vfs/cpp/connection.h>
#include <lib/vfs/cpp/flags.h>
#include <lib/vfs/cpp/internal/node_connection.h>
#include <zircon/assert.h>

namespace vfs {
namespace {

constexpr uint32_t kCommonAllowedFlags =
    fuchsia::io::OPEN_FLAG_DESCRIBE | fuchsia::io::OPEN_FLAG_NODE_REFERENCE;

}  // namespace

Node::Node() = default;

Node::~Node() = default;

zx_status_t Node::Close(Connection* connection) {
  RemoveConnection(connection);
  return ZX_OK;
}

zx_status_t Node::Sync() { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t Node::GetAttr(fuchsia::io::NodeAttributes* out_attributes) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Node::ValidateFlags(uint32_t flags) const {
  bool is_directory = IsDirectory();
  if (!is_directory && Flags::IsDirectory(flags)) {
    return ZX_ERR_NOT_DIR;
  }

  auto allowed_flags = kCommonAllowedFlags | GetAdditionalAllowedFlags();
  if (is_directory) {
    allowed_flags = allowed_flags | fuchsia::io::OPEN_FLAG_DIRECTORY;
  }

  auto prohibitive_flags = GetProhibitiveFlags();

  if ((flags & prohibitive_flags) != 0) {
    return ZX_ERR_INVALID_ARGS;
  }
  if ((flags & ~allowed_flags) != 0) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

uint32_t Node::GetAdditionalAllowedFlags() const { return 0; }

uint32_t Node::GetProhibitiveFlags() const { return 0; }

zx_status_t Node::SetAttr(uint32_t flags,
                          const fuchsia::io::NodeAttributes& attributes) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Node::Serve(uint32_t flags, zx::channel request,
                        async_dispatcher_t* dispatcher) {
  zx_status_t status = ValidateFlags(flags);
  if (status != ZX_OK) {
    SendOnOpenEventOnError(flags, std::move(request), status);
    return status;
  }
  std::unique_ptr<Connection> connection;
  status = CreateConnection(flags, &connection);
  if (status != ZX_OK) {
    SendOnOpenEventOnError(flags, std::move(request), status);
    return status;
  }
  status = connection->Bind(std::move(request), dispatcher);
  if (status == ZX_OK) {
    if (Flags::ShouldDescribe(flags)) {
      connection->SendOnOpenEvent(status);
    }
    AddConnection(std::move(connection));
  }  // can't send status as request object is gone.
  return status;
}

void Node::SendOnOpenEventOnError(uint32_t flags, zx::channel request,
                                  zx_status_t status) {
  ZX_DEBUG_ASSERT(status != ZX_OK);

  if (!Flags::ShouldDescribe(flags)) {
    return;
  }

  fuchsia_io_NodeOnOpenEvent msg;
  memset(&msg, 0, sizeof(msg));
  msg.hdr.ordinal = fuchsia_io_NodeOnOpenOrdinal;
  msg.s = status;
  request.write(0, &msg, sizeof(msg), nullptr, 0);
}

void Node::RemoveConnection(Connection* connection) {
  connections_.erase(std::find_if(
      connections_.begin(), connections_.end(),
      [connection](const auto& entry) { return entry.get() == connection; }));
}

void Node::AddConnection(std::unique_ptr<Connection> connection) {
  connections_.push_back(std::move(connection));
}

zx_status_t Node::CreateConnection(uint32_t flags,
                                   std::unique_ptr<Connection>* connection) {
  *connection = std::make_unique<internal::NodeConnection>(flags, this);
  return ZX_OK;
}

}  // namespace vfs
