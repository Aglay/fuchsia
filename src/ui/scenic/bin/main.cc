// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>

#include <trace-provider/provider.h>

#include "src/lib/fsl/syslogger/init.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/inspect_deprecated/inspect.h"
#include "src/ui/scenic/bin/app.h"

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;
  if (fsl::InitLoggerFromCommandLine(command_line, {"scenic"}) != ZX_OK)
    return 1;

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());
  std::unique_ptr<sys::ComponentContext> app_context(sys::ComponentContext::Create());

  // Set up an inspect_deprecated::Node to inject into the App.
  auto object_dir = component::ObjectDir(component::Object::Make("objects"));
  fidl::BindingSet<fuchsia::inspect::deprecated::Inspect> inspect_bindings;
  app_context->outgoing()->GetOrCreateDirectory("objects")->AddEntry(
      fuchsia::inspect::deprecated::Inspect::Name_,
      std::make_unique<vfs::Service>(inspect_bindings.GetHandler(object_dir.object().get())));

  scenic_impl::App app(std::move(app_context), inspect_deprecated::Node(std::move(object_dir)),
                       [&loop] { loop.Quit(); });

  loop.Run();
  FXL_LOG(INFO) << "Quit main Scenic loop.";

  return 0;
}
