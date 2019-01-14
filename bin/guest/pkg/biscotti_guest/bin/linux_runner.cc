// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/pkg/biscotti_guest/bin/linux_runner.h"

#include <fuchsia/guest/vmm/cpp/fidl.h>
#include <memory>

#include "lib/fxl/logging.h"
#include "lib/svc/cpp/service_provider_bridge.h"

namespace biscotti {

LinuxRunner::LinuxRunner()
    : context_(component::StartupContext::CreateFromStartupInfo()) {
  context_->outgoing().AddPublicService(bindings_.GetHandler(this));
}

zx_status_t LinuxRunner::Init(fxl::CommandLine cl) {
  return Guest::CreateAndStart(context_.get(), std::move(cl), &guest_);
}

void LinuxRunner::StartComponent(
    fuchsia::sys::Package application, fuchsia::sys::StartupInfo startup_info,
    fidl::InterfaceRequest<fuchsia::sys::ComponentController> controller) {
  AppLaunchRequest request = {
      std::move(application),
      std::move(startup_info),
      std::move(controller),
  };
  guest_->Launch(std::move(request));
}

}  // namespace biscotti
