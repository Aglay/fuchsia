// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iostream>

#include "application/lib/app/application_context.h"
#include "lib/ftl/command_line.h"
#include "lib/ftl/log_settings.h"
#include "lib/mtl/tasks/message_loop.h"

#include "app.h"

namespace {

const char kUsageString[] =
    "Options:\n"
    "  --verbose         : sets |min_log_level| to -1\n"
    "  --verbose=<level> : sets |min_log_level| to -level\n"
    "  --quiet           : sets |min_log_level| to +1 (LOG_WARNING)\n"
    "  --quiet=<level>   : sets |min_log_level| to +level\n"
    "  --log-file=<file> : sets |log_file| to file, uses default output if empty\n"
    "";

}  // namespace

int main(int argc, const char* argv[]) {
  auto cl = ftl::CommandLineFromArgcArgv(argc, argv);
  if (!ftl::SetLogSettingsFromCommandLine(cl)) {
    std::cout << kUsageString << std::endl;
    return EXIT_FAILURE;
  }

  mtl::MessageLoop message_loop;

  bluetooth_service::App app(app::ApplicationContext::CreateFromStartupInfo());

  message_loop.Run();

  return EXIT_SUCCESS;
}
