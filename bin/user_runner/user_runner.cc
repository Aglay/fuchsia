// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <trace-provider/provider.h>
#include <memory>

#include "lib/app/cpp/startup_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/device_runner/cobalt/cobalt.h"
#include "peridot/bin/user_runner/user_runner_impl.h"

fxl::AutoCall<fxl::Closure> SetupCobalt(
    const bool disable_statistics, async_t* async,
    fuchsia::sys::StartupContext* const startup_context) {
  if (disable_statistics) {
    return fxl::MakeAutoCall<fxl::Closure>([] {});
  }
  return modular::InitializeCobalt(async, startup_context);
}

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  const bool test = command_line.HasOption("test");

  fsl::MessageLoop loop;
  trace::TraceProvider trace_provider(loop.async());
  std::unique_ptr<fuchsia::sys::StartupContext> context =
      fuchsia::sys::StartupContext::CreateFromStartupInfo();

  fxl::AutoCall<fxl::Closure> cobalt_cleanup =
      SetupCobalt(test, std::move(loop.async()), context.get());

  modular::AppDriver<modular::UserRunnerImpl> driver(
      context->outgoing().deprecated_services(),
      std::make_unique<modular::UserRunnerImpl>(context.get(), test),
      [&loop, &cobalt_cleanup] {
        cobalt_cleanup.call();
        loop.QuitNow();
      });

  loop.Run();
  return 0;
}
