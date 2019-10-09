// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vnode.h"

#include <fuchsia/fshost/c/fidl.h>
#include <inttypes.h>
#include <lib/fidl-utils/bind.h>
#include <lib/memfs/cpp/vnode.h>

#include <fs/tracked-remote-dir.h>
#include <fs/vfs_types.h>

namespace devmgr {
namespace fshost {
namespace {

// A connection bespoke to the fshost Vnode, capable of serving fshost FIDL
// requests.
class Connection final : public fs::Connection {
 public:
  Connection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode, zx::channel channel,
             fs::VnodeConnectionOptions options);

 private:
  static const fuchsia_fshost_Registry_ops* Ops() {
    using Binder = fidl::Binder<Connection>;
    static const fuchsia_fshost_Registry_ops kFshostOps = {
        .RegisterFilesystem = Binder::BindMember<&Connection::RegisterFilesystem>,
    };
    return &kFshostOps;
  }

  Vnode& GetVnode() const;
  zx_status_t RegisterFilesystem(zx_handle_t channel, fidl_txn_t* txn);

  zx_status_t HandleFsSpecificMessage(fidl_msg_t* msg, fidl_txn_t* txn) final;
};

}  // namespace

Vnode::Vnode(async_dispatcher_t* dispatcher, fbl::RefPtr<fs::PseudoDir> filesystems)
    : filesystems_(std::move(filesystems)), filesystem_counter_(0), dispatcher_(dispatcher) {}

zx_status_t Vnode::AddFilesystem(zx::channel directory) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%" PRIu64 "", filesystem_counter_++);

  auto directory_vnode =
      fbl::AdoptRef<fs::TrackedRemoteDir>(new fs::TrackedRemoteDir(std::move(directory)));

  return directory_vnode->AddAsTrackedEntry(dispatcher_, filesystems_.get(), fbl::String(buf));
}

zx_status_t Vnode::ValidateOptions(fs::VnodeConnectionOptions options) {
  if (options.flags.directory) {
    return ZX_ERR_NOT_DIR;
  }
  return ZX_OK;
}

zx_status_t Vnode::GetAttributes(fs::VnodeAttributes* attr) {
  *attr = fs::VnodeAttributes();
  attr->mode = V_TYPE_FILE;
  attr->link_count = 1;
  return ZX_OK;
}

zx_status_t Vnode::Serve(fs::Vfs* vfs, zx::channel channel, fs::VnodeConnectionOptions options) {
  return vfs->ServeConnection(
      std::make_unique<Connection>(vfs, fbl::RefPtr(this), std::move(channel), options));
}

zx_status_t Vnode::GetNodeInfo([[maybe_unused]] fs::Rights, fuchsia_io_NodeInfo* info) {
  info->tag = fuchsia_io_NodeInfoTag_service;
  return ZX_OK;
}

Connection::Connection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode, zx::channel channel,
                       fs::VnodeConnectionOptions options)
    : fs::Connection(vfs, std::move(vnode), std::move(channel), options) {}

Vnode& Connection::GetVnode() const { return reinterpret_cast<Vnode&>(fs::Connection::GetVnode()); }

zx_status_t Connection::RegisterFilesystem(zx_handle_t channel, fidl_txn_t* txn) {
  zx::channel public_export(channel);
  zx_status_t status = GetVnode().AddFilesystem(std::move(public_export));
  return fuchsia_fshost_RegistryRegisterFilesystem_reply(txn, status);
}

zx_status_t Connection::HandleFsSpecificMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  fidl_message_header_t* hdr = reinterpret_cast<fidl_message_header_t*>(msg->bytes);
  // Depending on the state of the migration, GenOrdinal and Ordinal may be the
  // same value.  See FIDL-524.
  uint64_t ordinal = hdr->ordinal;
  if (ordinal == fuchsia_fshost_RegistryRegisterFilesystemOrdinal ||
      ordinal == fuchsia_fshost_RegistryRegisterFilesystemGenOrdinal) {
    return fuchsia_fshost_Registry_dispatch(this, txn, msg, Ops());
  }
  zx_handle_close_many(msg->handles, msg->num_handles);
  return ZX_ERR_NOT_SUPPORTED;
}

}  // namespace fshost
}  // namespace devmgr
