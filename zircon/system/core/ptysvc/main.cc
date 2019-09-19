// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fs-pty/service.h>
#include <lib/svc/outgoing.h>
#include <lib/zx/eventpair.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <fs/vnode.h>

#include "pty-server-vnode.h"
#include "pty-server.h"

// Each Open() on this Vnode redirects to a new PtyServerVnode
class PtyGeneratingVnode : public fs::Vnode {
 public:
  ~PtyGeneratingVnode() override = default;

  zx_status_t GetNodeInfo(uint32_t flags, fuchsia_io_NodeInfo* info) override {
    // This should only actually be seen by something querying with VNODE_REF_ONLY.
    info->tag = fuchsia_io_NodeInfoTag_service;
    return ZX_OK;
  }

  bool IsDirectory() const override { return false; }

  zx_status_t Open(uint32_t flags, fbl::RefPtr<Vnode>* out_redirect) override {
    fbl::RefPtr<PtyServer> server;
    zx_status_t status = PtyServer::Create(&server);
    if (status != ZX_OK) {
      return status;
    }
    *out_redirect = fbl::MakeRefCounted<PtyServerVnode>(std::move(server));
    return ZX_OK;
  }
};

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  async_dispatcher_t* dispatcher = loop.dispatcher();

  svc::Outgoing outgoing(dispatcher);
  zx_status_t status = outgoing.ServeFromStartupInfo();
  if (status != ZX_OK) {
    printf("console: outgoing.ServeFromStartupInfo() = %s\n", zx_status_get_string(status));
    return -1;
  }

  outgoing.svc_dir()->AddEntry("fuchsia.hardware.pty.Device",
                               fbl::MakeRefCounted<PtyGeneratingVnode>());

  status = loop.Run();
  ZX_ASSERT(status == ZX_OK);
  return 0;
}
