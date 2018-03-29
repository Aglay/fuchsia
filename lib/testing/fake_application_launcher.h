// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_TESTING_FAKE_APPLICATION_LAUNCHER_H_
#define PERIDOT_LIB_TESTING_FAKE_APPLICATION_LAUNCHER_H_

#include <map>
#include <string>

#include <fuchsia/cpp/component.h>

#include "lib/fidl/cpp/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/macros.h"

namespace modular {
namespace testing {

class FakeApplicationLauncher : public component::ApplicationLauncher {
 public:
  using ApplicationConnectorFn = std::function<void(
      component::ApplicationLaunchInfo,
      fidl::InterfaceRequest<component::ApplicationController>)>;

  // Registers an application located at "url" with a connector. When someone
  // tries to CreateApplication() with this |url|, the supplied |connector| is
  // called with the the ApplicationLaunchInfo and associated
  // ApplicationController request. The connector may implement the
  // |ApplicationLaunchInfo.services| and |ApplicationController| interfaces to
  // communicate with its connector and listen for application closing signals
  void RegisterApplication(std::string url, ApplicationConnectorFn connector);

 private:
  // Forwards this |CreateApplication| request to a registered connector, if an
  // associated one exists. If one is not registered for |launch_info.url|, then
  // this call is dropped.
  // |ApplicationLauncher|
  void CreateApplication(
      component::ApplicationLaunchInfo launch_info,
      fidl::InterfaceRequest<component::ApplicationController> controller)
      override;

  std::map<std::string, ApplicationConnectorFn> connectors_;
};

}  // namespace testing
}  // namespace modular

#endif  // PERIDOT_LIB_TESTING_FAKE_APPLICATION_LAUNCHER_H_
