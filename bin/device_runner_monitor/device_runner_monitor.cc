// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "lib/app/cpp/application_context.h"
#include <fuchsia/cpp/modular.h>
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"

namespace modular {

class DeviceRunnerMonitorApp : DeviceRunnerMonitor {
 public:
  DeviceRunnerMonitorApp()
      : app_context_(
            component::ApplicationContext::CreateFromStartupInfoNotChecked()) {
    app_context_->outgoing_services()->AddService<DeviceRunnerMonitor>(
        [this](f1dl::InterfaceRequest<DeviceRunnerMonitor> request) {
          bindings_.AddBinding(this, std::move(request));
        });
  }

 private:
  // |DeviceRunnerMonitor|
  void GetConnectionCount(const GetConnectionCountCallback& callback) override {
    callback(bindings_.size());
  }

  std::unique_ptr<component::ApplicationContext> app_context_;
  f1dl::BindingSet<DeviceRunnerMonitor> bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DeviceRunnerMonitorApp);
};

}  // namespace modular

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  modular::DeviceRunnerMonitorApp app;
  loop.Run();
  return 0;
}
