// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/power/statecontrol/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/modular/session/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fit/defer.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace-provider/provider.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/macros.h"
#include "src/modular/bin/basemgr/basemgr_impl.h"
#include "src/modular/bin/basemgr/cobalt/cobalt.h"
#include "src/modular/lib/modular_config/modular_config.h"
#include "src/modular/lib/modular_config/modular_config_accessor.h"
#include "src/modular/lib/modular_config/modular_config_constants.h"

fit::deferred_action<fit::closure> SetupCobalt(bool enable_cobalt, async_dispatcher_t* dispatcher,
                                               sys::ComponentContext* component_context) {
  if (!enable_cobalt) {
    return fit::defer<fit::closure>([] {});
  }
  return modular::InitializeCobalt(dispatcher, component_context);
};

std::unique_ptr<modular::BasemgrImpl> CreateBasemgrImpl(
    modular::ModularConfigAccessor config_accessor, sys::ComponentContext* component_context,
    async::Loop* loop) {
  fit::deferred_action<fit::closure> cobalt_cleanup = SetupCobalt(
      config_accessor.basemgr_config().enable_cobalt(), loop->dispatcher(), component_context);

  return std::make_unique<modular::BasemgrImpl>(
      std::move(config_accessor), component_context->svc(), component_context->outgoing(),
      component_context->svc()->Connect<fuchsia::sys::Launcher>(),
      component_context->svc()->Connect<fuchsia::ui::policy::Presenter>(),
      component_context->svc()->Connect<fuchsia::hardware::power::statecontrol::Admin>(),
      /*on_shutdown=*/
      [loop, cobalt_cleanup = std::move(cobalt_cleanup), component_context]() mutable {
        cobalt_cleanup.call();
        component_context->outgoing()->debug_dir()->RemoveEntry(modular_config::kBasemgrConfigName);
        loop->Quit();
      });
}

modular::ModularConfigAccessor ReadConfigFromNamespace() {
  auto config_reader = modular::ModularConfigReader::CreateFromNamespace();

  fuchsia::modular::session::ModularConfig modular_config;
  modular_config.set_basemgr_config(config_reader.GetBasemgrConfig());
  modular_config.set_sessionmgr_config(config_reader.GetSessionmgrConfig());

  return modular::ModularConfigAccessor(std::move(modular_config));
}

int main(int argc, const char** argv) {
  syslog::SetTags({"basemgr"});

  if (argc != 1) {
    std::cerr << "basemgr does not support arguments. Please use basemgr_launcher to "
              << "launch basemgr with custom configurations." << std::endl;
    return 1;
  }

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());
  std::unique_ptr<sys::ComponentContext> component_context(
      sys::ComponentContext::CreateAndServeOutgoingDirectory());

  // Read configuration from /config/data
  auto config_accessor = ReadConfigFromNamespace();

  auto basemgr_impl = CreateBasemgrImpl(std::move(config_accessor), component_context.get(), &loop);

  // NOTE: component_controller.events.OnDirectoryReady() is triggered when a
  // component's out directory has mounted. basemgr_launcher uses this signal
  // to determine when basemgr has completed initialization so it can detach
  // and stop itself. When basemgr_launcher is used, it's responsible for
  // providing basemgr a configuration file. To ensure we don't shutdown
  // basemgr_launcher too early, we need additions to out/ to complete after
  // configurations have been parsed.
  component_context->outgoing()->debug_dir()->AddEntry(
      modular_config::kBasemgrConfigName,
      std::make_unique<vfs::Service>([basemgr_impl = basemgr_impl.get()](
                                         zx::channel request, async_dispatcher_t* /* unused */) {
        basemgr_impl->Connect(
            fidl::InterfaceRequest<fuchsia::modular::internal::BasemgrDebug>(std::move(request)));
      }));

  loop.Run();

  // The loop will run until graceful shutdown is complete so returning SUCCESS here indicates that.
  return EXIT_SUCCESS;
}
