// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/fdio.h>
#include <lib/syslog/global.h>
#include <zircon/status.h>

#include <string>

#include "dockyard_proxy.h"
#include "dockyard_proxy_grpc.h"
#include "dockyard_proxy_local.h"
#include "harvester.h"
#include "root_resource.h"
#include "src/lib/fsl/syslogger/init.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_number_conversions.h"

int main(int argc, char** argv) {
  constexpr int EXIT_CODE_OK = 0;
  // A broad 'something went wrong' error.
  constexpr int EXIT_CODE_GENERAL_ERROR = 1;

  // The wip number is incremented arbitrarily.
  // TODO(fxb/44) replace wip number with real version number.
  constexpr char VERSION_OUTPUT[] =
      "System Monitor Harvester 20191211\n"
      "- memory_digest\n"
      "+ separate cpu and memory gather\n";

  // Command line options.
  constexpr char COMMAND_LOCAL[] = "local";
  constexpr char COMMAND_VERSION[] = "version";

  bool use_grpc = true;

  // Parse command line.
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fsl::InitLoggerFromCommandLine(command_line, {"harvester"});

  FX_LOGS(INFO) << VERSION_OUTPUT;

  if (command_line.HasOption(COMMAND_VERSION)) {
    std::cout << VERSION_OUTPUT << std::endl;
    exit(EXIT_CODE_OK);
  }
  if (command_line.HasOption(COMMAND_LOCAL)) {
    FX_LOGS(INFO) << "Option: local only, not using transport to Dockyard.";
    use_grpc = false;
  }

  // Set up.
  std::unique_ptr<harvester::DockyardProxy> dockyard_proxy;
  if (use_grpc) {
    const auto& positional_args = command_line.positional_args();
    if (positional_args.empty()) {
      // TODO(fxb/30): Adhere to CLI tool requirements for --help.
      std::cerr << "Please specify an IP:Port, such as localhost:50051"
                << std::endl;
      exit(EXIT_CODE_GENERAL_ERROR);
    }

    // TODO(fxb/32): This channel isn't authenticated
    // (InsecureChannelCredentials()).
    dockyard_proxy =
        std::make_unique<harvester::DockyardProxyGrpc>(grpc::CreateChannel(
            positional_args[0], grpc::InsecureChannelCredentials()));

    if (!dockyard_proxy) {
      FX_LOGS(ERROR) << "unable to create dockyard_proxy";
      exit(EXIT_CODE_GENERAL_ERROR);
    }
    harvester::DockyardProxyStatus status = dockyard_proxy->Init();
    if (status != harvester::DockyardProxyStatus::OK) {
      FX_LOGS(ERROR) << harvester::DockyardErrorString("Init", status);
      exit(EXIT_CODE_GENERAL_ERROR);
    }
  } else {
    dockyard_proxy = std::make_unique<harvester::DockyardProxyLocal>();
  }

  zx_handle_t root_resource;
  zx_status_t ret = harvester::GetRootResource(&root_resource);
  if (ret != ZX_OK) {
    exit(EXIT_CODE_GENERAL_ERROR);
  }

  // Note: Neither of the following loops are "fast" or "slow" on their own.
  //       It's just a matter of what we choose to run on them.
  // Create a separate loop for quick calls (don't run long running functions on
  // this loop).
  // The "slow" loop is used for potentially long running calls.
  async::Loop slow_calls_loop(&kAsyncLoopConfigAttachToCurrentThread);
  async::Loop fast_calls_loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  // The loop that runs quick calls is in a separate thread.
  zx_status_t status = fast_calls_loop.StartThread("fast-calls-thread");
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "fast_calls_loop.StartThread failed " << status;
    exit(EXIT_CODE_GENERAL_ERROR);
  }
  FX_LOGS(INFO) << "main thread " << pthread_self();
  harvester::Harvester harvester(root_resource, std::move(dockyard_proxy));
  harvester.GatherDeviceProperties();
  harvester.GatherFastData(fast_calls_loop.dispatcher());
  harvester.GatherSlowData(slow_calls_loop.dispatcher());
  // The slow_calls_thread that runs heavier calls takes over this thread.
  slow_calls_loop.Run();
  fast_calls_loop.Quit();

  FX_LOGS(INFO) << "System Monitor Harvester - exiting";
  return 0;
}
