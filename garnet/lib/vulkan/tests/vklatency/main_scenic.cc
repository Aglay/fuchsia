// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/trace-provider/provider.h>
#include <lib/ui/base_view/cpp/view_provider_component_transitional.h>

#include "garnet/lib/vulkan/tests/vklatency/image_pipe_view.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());

  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;

  const bool protected_output = command_line.HasOption("protected_output");
  scenic::ViewProviderComponentTransitional component(
      [protected_output](scenic::ViewContextTransitional view_context) {
        return std::make_unique<examples::ImagePipeView>(std::move(view_context), protected_output);
      },
      &loop);
  loop.Run();

  return 0;
}
