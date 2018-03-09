// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user/agent_launcher.h"

#include "lib/fxl/logging.h"

namespace maxwell {
namespace {

constexpr char kEnvironmentLabel[] = "agent";

}  // namespace

app::Services AgentLauncher::StartAgent(
    const std::string& url,
    std::unique_ptr<app::ApplicationEnvironmentHost> env_host) {
  f1dl::InterfaceHandle<app::ApplicationEnvironmentHost> agent_host_handle =
      agent_host_bindings_.AddBinding(std::move(env_host));

  app::ApplicationEnvironmentPtr agent_env;
  environment_->CreateNestedEnvironment(std::move(agent_host_handle),
                                        agent_env.NewRequest(), NULL,
                                        kEnvironmentLabel);

  app::ApplicationLauncherPtr agent_launcher;
  agent_env->GetApplicationLauncher(agent_launcher.NewRequest());

  auto launch_info = app::ApplicationLaunchInfo::New();
  launch_info->url = url;
  app::Services services;
  launch_info->directory_request = services.NewRequest();
  FXL_LOG(INFO) << "Starting Maxwell agent " << url;
  agent_launcher->CreateApplication(std::move(launch_info), NULL);
  return services;
}

}  // namespace maxwell
