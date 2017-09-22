// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <trace-provider/provider.h>

#include "garnet/examples/escher/common/demo_harness_fuchsia.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/log_settings_command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fsl/tasks/message_loop.h"

#include "garnet/bin/ui/scene_manager/displays/display_manager.h"
#include "garnet/bin/ui/scene_manager/scene_manager_app.h"

using namespace scene_manager;

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;

  SceneManagerApp::Params params;
  if (!params.Setup(command_line))
    return 1;

  fsl::MessageLoop loop;
  trace::TraceProvider trace_provider(loop.async());

  std::unique_ptr<SceneManagerApp> scene_manager_app;

  // Don't initialize Vulkan and the SceneManagerApp until display is ready.
  DisplayManager display_manager;
  display_manager.WaitForDefaultDisplay([&scene_manager_app, &params,
                                         &display_manager]() {
    Display* display = display_manager.default_display();
    if (!display) {
      FXL_LOG(ERROR) << "No default display, SceneManager exiting";
      fsl::MessageLoop::GetCurrent()->PostQuitTask();
      return;
    }

    // Only enable Vulkan validation layers when in debug mode.
    escher::VulkanInstance::Params instance_params({{}, {}, true});
#if !defined(NDEBUG)
    instance_params.layer_names.insert("VK_LAYER_LUNARG_standard_validation");
#endif

    auto harness = DemoHarness::New(
        DemoHarness::WindowParams{"Mozart SceneManager", display->width(),
                                  display->height(), 2, false},
        std::move(instance_params));

    app::ApplicationContext* application_context =
        static_cast<DemoHarnessFuchsia*>(harness.get())->application_context();
    scene_manager_app = std::make_unique<SceneManagerApp>(
        application_context, &params, &display_manager, std::move(harness));
  });

  loop.Run();
  return 0;
}
