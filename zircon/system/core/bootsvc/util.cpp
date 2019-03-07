// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util.h"

#include <fs/connection.h>
#include <zircon/process.h>
#include <zircon/processargs.h>

namespace bootsvc {

const char* const kLastPanicFilePath = "log/last-panic.txt";

fbl::Vector<zx::vmo> RetrieveBootdata() {
    fbl::Vector<zx::vmo> vmos;
    zx::vmo vmo;
    for (unsigned n = 0;
         vmo.reset(zx_take_startup_handle(PA_HND(PA_VMO_BOOTDATA, n))), vmo.is_valid();
         n++) {
        vmos.push_back(std::move(vmo));
    }
    return vmos;
}

zx_status_t CreateVnodeConnection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode, zx::channel* out) {
    zx::channel local, remote;
    zx_status_t status = zx::channel::create(0, &local, &remote);
    if (status != ZX_OK) {
        return status;
    }

    auto conn = fbl::make_unique<fs::Connection>(vfs, vnode, std::move(local),
                                                 ZX_FS_FLAG_DIRECTORY | ZX_FS_RIGHT_READABLE);
    status = vfs->ServeConnection(std::move(conn));
    if (status != ZX_OK) {
        return status;
    }
    *out = std::move(remote);
    return ZX_OK;
}

} // namespace bootsvc
