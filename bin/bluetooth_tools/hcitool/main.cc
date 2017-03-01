// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <iostream>

#include <fcntl.h>
#include <sys/stat.h>

#include "apps/bluetooth/common/byte_buffer.h"
#include "apps/bluetooth/hci/command_packet.h"
#include "apps/bluetooth/hci/hci.h"
#include "apps/bluetooth/hci/transport.h"
#include "lib/ftl/command_line.h"
#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/log_settings.h"
#include "lib/ftl/strings/string_printf.h"
#include "lib/mtl/tasks/message_loop.h"

#include "command_dispatcher.h"
#include "commands.h"

using namespace bluetooth;

namespace {

const char kUsageString[] =
    "Usage: hcitool [--dev=<bt-hci-dev>] cmd...\n"
    "    e.g. hcitool reset";

const char kDefaultHCIDev[] = "/dev/class/bt-hci/000";

}  // namespace

int main(int argc, char* argv[]) {
  auto cl = ftl::CommandLineFromArgcArgv(argc, argv);

  if (cl.HasOption("help", nullptr)) {
    std::cout << kUsageString << std::endl;
    return EXIT_SUCCESS;
  }

  // By default suppress all log messages below the LOG_ERROR level.
  ftl::LogSettings log_settings;
  log_settings.min_log_level = ftl::LOG_ERROR;
  if (!ftl::ParseLogSettings(cl, &log_settings)) {
    std::cout << kUsageString << std::endl;
    return EXIT_FAILURE;
  }

  ftl::SetLogSettings(log_settings);

  std::string hci_dev_path = kDefaultHCIDev;
  if (cl.GetOptionValue("dev", &hci_dev_path) && hci_dev_path.empty()) {
    std::cout << "Empty device path not allowed" << std::endl;
    return EXIT_FAILURE;
  }

  // TODO(armansito): Add a command-line option for passing a bt-hci device
  // path.
  ftl::UniqueFD hci_dev(open(hci_dev_path.c_str(), O_RDWR));
  if (!hci_dev.is_valid()) {
    std::perror("Failed to open HCI device");
    return EXIT_FAILURE;
  }

  hci::Transport hci(std::move(hci_dev));
  hci.Initialize();
  mtl::MessageLoop message_loop;

  hcitool::CommandDispatcher handler_map(hci.command_channel(), message_loop.task_runner());
  RegisterCommands(&handler_map);

  if (cl.positional_args().empty() || cl.positional_args()[0] == "help") {
    handler_map.DescribeAllCommands();
    return EXIT_SUCCESS;
  }

  auto complete_cb = [&message_loop] { message_loop.PostQuitTask(); };

  bool cmd_found;
  if (!handler_map.ExecuteCommand(cl.positional_args(), complete_cb, &cmd_found)) {
    if (!cmd_found) std::cout << "Unknown command: " << cl.positional_args()[0] << std::endl;
    return EXIT_FAILURE;
  }

  message_loop.Run();

  return EXIT_SUCCESS;
}
