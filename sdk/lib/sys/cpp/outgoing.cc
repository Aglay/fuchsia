// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/sys/cpp/outgoing.h>

#include <utility>

#include <zircon/process.h>
#include <zircon/processargs.h>

namespace sys {

Outgoing::Outgoing() : root_(std::make_unique<vfs::PseudoDir>()) {
  auto dir = std::make_unique<vfs::PseudoDir>();
  public_ = dir.get();
  root_->AddEntry("public", std::move(dir));
}

Outgoing::~Outgoing() = default;

zx_status_t Outgoing::Serve(zx::channel directory_request,
                            async_dispatcher_t* dispatcher) {
  return root_->Serve(fuchsia::io::OPEN_RIGHT_READABLE,
                      std::move(directory_request), dispatcher);
}

zx_status_t Outgoing::ServeFromStartupInfo(async_dispatcher_t* dispatcher) {
  return Serve(zx::channel(zx_take_startup_handle(PA_DIRECTORY_REQUEST)),
               dispatcher);
}

}  // namespace sys
