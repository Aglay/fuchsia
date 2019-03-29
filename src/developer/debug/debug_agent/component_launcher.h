// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <string>

#include <fuchsia/sys/cpp/fidl.h>
#include <zircon/types.h>

#include "lib/sys/cpp/service_directory.h"
#include "src/developer/debug/shared/component_utils.h"

namespace debug_agent {

// When preparing a component, this is information the debugger will use in
// order to be able to attach to the newly starting process.
struct LaunchComponentDescription {
  std::string url;
  std::string process_name;
  std::string filter;
};

// Class designed to help setup a component and then launch it. These setps are
// necessary because the agent needs some information about how the component
// will be launch before it actually launches it. This is because the debugger
// will set itself to "catch" the component when it starts as a process.
class ComponentLauncher {
 public:
  explicit ComponentLauncher(std::shared_ptr<sys::ServiceDirectory> services);

  // Will fail if |argv| is invalid. The first element should be the component
  // url needed to launch.
  zx_status_t Prepare(std::vector<std::string> argv,
                      LaunchComponentDescription* out);

  // The launcher has to be already successfully prepared.
  // The lifetime of the controller is bound to the lifetime of the component.
  fuchsia::sys::ComponentControllerPtr Launch();

  const LaunchComponentDescription& desc() const { return desc_; }

 private:
  std::shared_ptr<sys::ServiceDirectory> services_;
  LaunchComponentDescription desc_;
  std::vector<std::string> argv_;
};

}  // namespace debug_agent
