// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "registry_vnode.h"

#include <fuchsia/fshost/llcpp/fidl.h>
#include <inttypes.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/memfs/cpp/vnode.h>

#include "src/lib/storage/vfs/cpp/service.h"
#include "src/lib/storage/vfs/cpp/tracked_remote_dir.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"

namespace devmgr {
namespace fshost {

RegistryVnode::RegistryVnode(async_dispatcher_t* dispatcher, fbl::RefPtr<fs::PseudoDir> filesystems)
    : fs::Service([dispatcher, this](zx::channel server_end) {
        return fidl::BindSingleInFlightOnly(dispatcher, std::move(server_end), this);
      }),
      filesystems_(std::move(filesystems)),
      dispatcher_(dispatcher) {}

zx_status_t RegistryVnode::AddFilesystem(fidl::ClientEnd<fuchsia_io::Directory> directory) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%" PRIu64 "", filesystem_counter_++);

  auto directory_vnode = fbl::MakeRefCounted<fs::TrackedRemoteDir>(std::move(directory));

  return directory_vnode->AddAsTrackedEntry(dispatcher_, filesystems_.get(), fbl::String(buf));
}

void RegistryVnode::RegisterFilesystem(fidl::ClientEnd<fuchsia_io::Directory> public_export,
                                       RegisterFilesystemCompleter::Sync& completer) {
  completer.Reply(AddFilesystem(std::move(public_export)));
}

}  // namespace fshost
}  // namespace devmgr
