// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <memory>
#include <string>

#include <hello_world_module/cpp/fidl.h>
#include "lib/app/cpp/startup_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/macros.h"
#include "lib/svc/cpp/services.h"

namespace {

class HelloAppParent {
 public:
  explicit HelloAppParent(component::StartupContext* context,
                          fxl::CommandLine command_line) {
    component::LaunchInfo launch_info;
    const std::vector<std::string>& args = command_line.positional_args();
    if (args.empty()) {
      launch_info.url = "hello_app_child";
    } else {
      launch_info.url = args[0];
      for (size_t i = 1; i < args.size(); ++i) {
        launch_info.arguments.push_back(args[i]);
      }
    }
    launch_info.directory_request = child_services_.NewRequest();
    context->launcher()->CreateApplication(std::move(launch_info),
                                           child_.NewRequest());

    child_services_.ConnectToService(hello_.NewRequest());

    DoIt("hello");
    DoIt("goodbye");
  }

  // Called by AppDriver.
  void Terminate(const std::function<void()>& done) { done(); }

 private:
  void DoIt(const std::string& request) {
    hello_->Say(request, [request](fidl::StringPtr response) {
      printf("%s --> %s\n", request.c_str(), response.get().c_str());
    });
  }

  component::ComponentControllerPtr child_;
  component::Services child_services_;
  hello_world_module::HelloPtr hello_;

  FXL_DISALLOW_COPY_AND_ASSIGN(HelloAppParent);
};

}  // namespace

int main(int argc, const char** argv) {
  fsl::MessageLoop loop;
  auto context = component::StartupContext::CreateFromStartupInfo();
  fuchsia::modular::AppDriver<HelloAppParent> driver(
      context->outgoing().deprecated_services(),
      std::make_unique<HelloAppParent>(
          context.get(), fxl::CommandLineFromArgcArgv(argc, argv)),
      [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
