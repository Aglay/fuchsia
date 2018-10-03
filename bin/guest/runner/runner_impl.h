// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_RUNNER_RUNNER_IMPL_H_
#define GARNET_BIN_GUEST_RUNNER_RUNNER_IMPL_H_

#include <fuchsia/sys/cpp/fidl.h>

#include "lib/component/cpp/startup_context.h"
#include "lib/fidl/cpp/binding_set.h"

namespace guest_runner {

class RunnerImpl : public fuchsia::sys::Runner {
 public:
  RunnerImpl();

  RunnerImpl(const RunnerImpl&) = delete;
  RunnerImpl& operator=(const RunnerImpl&) = delete;

 private:
  // |fuchsia::sys::Runner|
  void StartComponent(
      fuchsia::sys::Package application, fuchsia::sys::StartupInfo startup_info,
      ::fidl::InterfaceRequest<fuchsia::sys::ComponentController> controller)
      override;

  std::unique_ptr<component::StartupContext> context_;
  fuchsia::sys::LauncherPtr launcher_;
  fidl::BindingSet<fuchsia::sys::Runner> bindings_;
};

}  // namespace guest_runner

#endif  // GARNET_BIN_GUEST_RUNNER_RUNNER_IMPL_H_
