// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <memory>
#include <string>

#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/macros.h"
#include "lib/svc/cpp/services.h"
#include "peridot/examples/hello_world_cpp/hello.fidl.h"

using examples::HelloPtr;

namespace {

class HelloAppParent {
 public:
  explicit HelloAppParent(component::ApplicationContext* app_context,
                          fxl::CommandLine command_line) {
    auto launch_info = component::ApplicationLaunchInfo::New();
    const std::vector<std::string>& args = command_line.positional_args();
    if (args.empty()) {
      launch_info->url = "hello_app_child";
    } else {
      launch_info->url = args[0];
      for (size_t i = 1; i < args.size(); ++i) {
        launch_info->arguments.push_back(args[i]);
      }
    }
    launch_info->directory_request = child_services_.NewRequest();
    app_context->launcher()->CreateApplication(std::move(launch_info),
                                               child_.NewRequest());

    child_services_.ConnectToService(hello_.NewRequest());

    DoIt("hello");
    DoIt("goodbye");
  }

  // Called by AppDriver.
  void Terminate(const std::function<void()>& done) { done(); }

 private:
  void DoIt(const std::string& request) {
    hello_->Say(request, [request](const f1dl::String& response) {
      printf("%s --> %s\n", request.c_str(), response.get().c_str());
    });
  }

  component::ApplicationControllerPtr child_;
  component::Services child_services_;
  HelloPtr hello_;

  FXL_DISALLOW_COPY_AND_ASSIGN(HelloAppParent);
};

}  // namespace

int main(int argc, const char** argv) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<HelloAppParent> driver(
      app_context->outgoing_services(),
      std::make_unique<HelloAppParent>(
          app_context.get(), fxl::CommandLineFromArgcArgv(argc, argv)),
      [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
