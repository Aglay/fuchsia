// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/channel.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>

#include <cstdlib>
#include <memory>

#include "src/developer/feedback/feedback_agent/data_provider.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/syslog/cpp/logger.h"

int main(int argc, const char** argv) {
  syslog::InitLogger({"feedback"});

  FXL_CHECK(argc == 2) << "feedback_agent is supposed to spawn us with two arguments";
  const std::string process_identifier = fxl::StringPrintf("%s (connection %s)", argv[0], argv[1]);
  FX_LOGS(INFO) << "Client opened a new connection to fuchsia.feedback.DataProvider. Spawned "
                << process_identifier;

  // This process is spawned by feedback_agent, which forwards it the incoming request through
  // PA_USER0.
  fidl::InterfaceRequest<fuchsia::feedback::DataProvider> request(
      zx::channel(zx_take_startup_handle(PA_HND(PA_USER0, 0))));
  if (!request.is_valid()) {
    FX_LOGS(ERROR) << "Invalid incoming fuchsia.feedback.DataProvider request";
    return EXIT_FAILURE;
  }

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto context = sys::ComponentContext::Create();
  std::unique_ptr<feedback::DataProvider> data_provider =
      feedback::DataProvider::TryCreate(loop.dispatcher(), context->svc());
  if (!data_provider) {
    return EXIT_FAILURE;
  }

  fidl::Binding<fuchsia::feedback::DataProvider> binding(data_provider.get());
  // TODO(DX-1497): in addition to exiting the process when the connection is closed, we should have
  // an internal timeout since the last call and exit the process then in case clients don't close
  // the connection themselves.
  binding.set_error_handler([&loop, &process_identifier](zx_status_t status) {
    loop.Shutdown();
    // We exit successfully when the client closes the connection.
    if (status == ZX_ERR_PEER_CLOSED) {
      FX_LOGS(INFO) << "Client closed the connection to fuchsia.feedback.DataProvider. Exiting "
                    << process_identifier;
      exit(0);
    } else {
      FX_PLOGS(ERROR, status) << "Received channel error. Exiting " << process_identifier;
      exit(1);
    }
  });
  binding.Bind(std::move(request));

  loop.Run();

  return EXIT_SUCCESS;
}
