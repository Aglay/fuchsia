// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/device/stream_impl.h"

#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/camera/bin/device/messages.h"
#include "src/camera/bin/device/util.h"

StreamImpl::StreamImpl(fidl::InterfaceHandle<fuchsia::camera2::Stream> legacy_stream,
                       fidl::InterfaceRequest<fuchsia::camera3::Stream> request,
                       fit::closure on_no_clients)
    : loop_(&kAsyncLoopConfigNoAttachToCurrentThread), on_no_clients_(std::move(on_no_clients)) {
  ZX_ASSERT(legacy_stream_.Bind(std::move(legacy_stream), loop_.dispatcher()) == ZX_OK);
  legacy_stream_.set_error_handler(fit::bind_member(this, &StreamImpl::OnLegacyStreamDisconnected));
  auto client = std::make_unique<Client>(*this, client_id_next_, std::move(request));
  clients_.emplace(client_id_next_++, std::move(client));
  ZX_ASSERT(loop_.StartThread("Camera Stream Thread") == ZX_OK);
}

StreamImpl::~StreamImpl() {
  Unbind(legacy_stream_);
  loop_.Quit();
  loop_.JoinThreads();
}

void StreamImpl::OnLegacyStreamDisconnected(zx_status_t status) {
  FX_PLOGS(ERROR, status) << "Legacy Stream disconnected unexpectedly.";
  clients_.clear();
  on_no_clients_();
}

void StreamImpl::PostRemoveClient(uint64_t id) {
  async::PostTask(loop_.dispatcher(), [=]() {
    clients_.erase(id);
    if (clients_.empty()) {
      on_no_clients_();
    }
  });
}
