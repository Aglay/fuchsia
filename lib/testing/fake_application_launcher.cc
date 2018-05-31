// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/testing/fake_application_launcher.h"

namespace fuchsia {
namespace modular {
namespace testing {

void FakeApplicationLauncher::CreateApplication(
    fuchsia::sys::LaunchInfo launch_info,
    fidl::InterfaceRequest<fuchsia::sys::ComponentController> controller) {
  auto it = connectors_.find(launch_info.url);
  if (it != connectors_.end()) {
    it->second(std::move(launch_info), std::move(controller));
  }
}

void FakeApplicationLauncher::RegisterApplication(
    std::string url,
    ApplicationConnectorFn connector) {
  connectors_[url] = connector;
}

}  // namespace testing
}  // namespace modular
}  // namespace fuchsia
