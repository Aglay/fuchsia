// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/vfs/cpp/internal/directory_connection.h>

#include <utility>

#include <lib/vfs/cpp/directory.h>

namespace vfs {
namespace internal {

DirectoryConnection::DirectoryConnection(uint32_t flags, vfs::Directory* vn)
    : Connection(flags), vn_(vn), binding_(this) {}

DirectoryConnection::~DirectoryConnection() = default;

zx_status_t DirectoryConnection::Bind(zx::channel request,
                                      async_dispatcher_t* dispatcher) {
  zx_status_t status = binding_.Bind(std::move(request), dispatcher);
  if (status != ZX_OK) {
    return status;
  }
  binding_.set_error_handler(
      [this](zx_status_t status) { vn_->RemoveConnection(this); });
  return ZX_OK;
}

void DirectoryConnection::Clone(
    uint32_t flags, fidl::InterfaceRequest<fuchsia::io::Node> object) {
  Connection::Clone(vn_, flags, std::move(object), binding_.dispatcher());
}

void DirectoryConnection::Close(CloseCallback callback) {
  Connection::Close(vn_, std::move(callback));
}

void DirectoryConnection::Describe(DescribeCallback callback) {
  Connection::Describe(vn_, std::move(callback));
}

void DirectoryConnection::Sync(SyncCallback callback) {
  Connection::Sync(vn_, std::move(callback));
}

void DirectoryConnection::GetAttr(GetAttrCallback callback) {
  Connection::GetAttr(vn_, std::move(callback));
}

void DirectoryConnection::SetAttr(uint32_t flags,
                                  fuchsia::io::NodeAttributes attributes,
                                  SetAttrCallback callback) {
  Connection::SetAttr(vn_, flags, attributes, std::move(callback));
}

void DirectoryConnection::Ioctl(uint32_t opcode, uint64_t max_out,
                                std::vector<zx::handle> handles,
                                std::vector<uint8_t> in,
                                IoctlCallback callback) {
  Connection::Ioctl(vn_, opcode, max_out, std::move(handles), std::move(in),
                    std::move(callback));
}

void DirectoryConnection::Open(
    uint32_t flags, uint32_t mode, std::string path,
    fidl::InterfaceRequest<fuchsia::io::Node> object) {}

void DirectoryConnection::Unlink(::std::string path, UnlinkCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED);
}

void DirectoryConnection::ReadDirents(uint64_t max_bytes,
                                      ReadDirentsCallback callback) {}

void DirectoryConnection::Rewind(RewindCallback callback) {}

void DirectoryConnection::GetToken(GetTokenCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED, zx::handle());
}

void DirectoryConnection::Rename(::std::string src, zx::handle dst_parent_token,
                                 std::string dst, RenameCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED);
}

void DirectoryConnection::Link(::std::string src, zx::handle dst_parent_token,
                               std::string dst, LinkCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED);
}

void DirectoryConnection::Watch(uint32_t mask, uint32_t options,
                                zx::channel watcher, WatchCallback callback) {
  // TODO: Implement watch.
}

}  // namespace internal
}  // namespace vfs
