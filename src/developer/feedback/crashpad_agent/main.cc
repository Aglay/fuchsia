// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/inspect/cpp/vmo/types.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/inspect/cpp/component.h>
#include <lib/timekeeper/system_clock.h>

#include <utility>

#include "src/developer/feedback/crashpad_agent/crashpad_agent.h"
#include "src/developer/feedback/crashpad_agent/info/info_context.h"
#include "src/lib/syslog/cpp/logger.h"

int main(int argc, const char** argv) {
  syslog::InitLogger({"feedback"});

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto context = sys::ComponentContext::Create();

  const timekeeper::SystemClock clock;

  auto inspector = std::make_unique<sys::ComponentInspector>(context.get());
  inspect::Node& root_node = inspector->root();

  auto info_context =
      std::make_shared<feedback::InfoContext>(&root_node, clock, loop.dispatcher(), context->svc());

  std::unique_ptr<feedback::CrashpadAgent> agent = feedback::CrashpadAgent::TryCreate(
      loop.dispatcher(), context->svc(), clock, std::move(info_context));
  if (!agent) {
    return EXIT_FAILURE;
  }

  // fuchsia.feedback.CrashReporter
  context->outgoing()->AddPublicService(
      ::fidl::InterfaceRequestHandler<fuchsia::feedback::CrashReporter>(
          [&agent](::fidl::InterfaceRequest<fuchsia::feedback::CrashReporter> request) {
            agent->HandleCrashReporterRequest(std::move(request));
          }));

  loop.Run();

  return EXIT_SUCCESS;
}
