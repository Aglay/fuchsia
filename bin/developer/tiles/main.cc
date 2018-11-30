// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ui/policy/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <trace-provider/provider.h>
#include <zx/eventpair.h>

#include "garnet/bin/developer/tiles/tiles.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/string_number_conversions.h"

void Usage() {
  printf(
      "Usage: tiles [--border=...]\n"
      "\n"
      "Tiles displays a set of views as tiles. Add or remove tiles with\n"
      "the 'tiles_ctl' command line utility or connecting to the\n"
      "fuchsia.developer.tiles.Tiles FIDL API exposed by this program\n"
      "\n"
      "Options:\n"
      "  --border=<integer>  Border (in pixels) around each tile\n"
      "  --input_path=[old|new]\n");
}

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  trace::TraceProvider trace_provider(loop.dispatcher());

  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (command_line.HasOption("h") || command_line.HasOption("help")) {
    Usage();
    return 0;
  }

  auto border_arg = command_line.GetOptionValueWithDefault("border", "10");
  int border = fxl::StringToNumber<int>(border_arg);

  bool input_path = true;
  {
    auto input_path_arg =
        command_line.GetOptionValueWithDefault("input_path", "old");
    input_path = input_path_arg != "new";
    FXL_LOG(INFO) << "Tiles requesting input delivery by: "
                  << (input_path ? "ViewManager" : "Scenic");
  }

  zx::eventpair view_owner_token, view_token;
  if (zx::eventpair::create(0u, &view_owner_token, &view_token) != ZX_OK)
    FXL_NOTREACHED() << "failed to create tokens.";

  // Create tiles with a token for its root view.
  auto startup_context = component::StartupContext::CreateFromStartupInfo();
  tiles::Tiles tiles(startup_context.get(), std::move(view_token),
                     command_line.positional_args(), border);

  // Ask the presenter to display it.
  auto presenter =
      startup_context
          ->ConnectToEnvironmentService<fuchsia::ui::policy::Presenter>();
  presenter->Present2(std::move(view_owner_token), nullptr);
  presenter->HACK_SetInputPath(input_path);

  loop.Run();
  return 0;
}
