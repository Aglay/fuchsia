// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "managed_environment.h"

namespace netemul {

using component::testing::EnclosingEnvironment;
using component::testing::EnvironmentServices;

ManagedEnvironment::Ptr ManagedEnvironment::CreateRoot(
    const fuchsia::sys::EnvironmentPtr& parent,
    const SandboxEnv::Ptr& sandbox_env, Options options) {
  auto ret = ManagedEnvironment::Ptr(new ManagedEnvironment(sandbox_env));
  ret->Create(parent, std::move(options));
  return ret;
}

ManagedEnvironment::ManagedEnvironment(const SandboxEnv::Ptr& sandbox_env)
    : sandbox_env_(sandbox_env) {}
component::testing::EnclosingEnvironment& ManagedEnvironment::environment() {
  return *env_;
}

void ManagedEnvironment::GetLauncher(
    ::fidl::InterfaceRequest<::fuchsia::sys::Launcher> launcher) {
  launcher_->Bind(std::move(launcher));
}

void ManagedEnvironment::CreateChildEnvironment(
    fidl::InterfaceRequest<FManagedEnvironment> me, Options options) {
  ManagedEnvironment::Ptr np(new ManagedEnvironment(sandbox_env_));
  fuchsia::sys::EnvironmentPtr env;
  env_->ConnectToService(env.NewRequest());
  np->Create(env, std::move(options), this);
  np->bindings_.AddBinding(np.get(), std::move(me));

  children_.emplace_back(std::move(np));
}

void ManagedEnvironment::Create(const fuchsia::sys::EnvironmentPtr& parent,
                                ManagedEnvironment::Options options,
                                const ManagedEnvironment* managed_parent) {
  auto services = EnvironmentServices::Create(parent);

  loggers_ = std::make_unique<ManagedLoggerCollection>(options.name);

  // add network context service:
  services->AddService(sandbox_env_->network_context().GetHandler());

  // add Bus service:
  services->AddService(sandbox_env_->bus_manager().GetHandler());

  // add managed environment itself as a handler
  services->AddService(bindings_.GetHandler(this));

  // prepare service configurations:
  service_config_.clear();
  if (options.inherit_parent_launch_services && managed_parent != nullptr) {
    for (const auto& a : managed_parent->service_config_) {
      LaunchService clone;
      a.Clone(&clone);
      service_config_.push_back(std::move(clone));
    }
  }

  std::move(options.services.begin(), options.services.end(),
            std::back_inserter(service_config_));

  // push all the allowable launch services:
  for (const auto& svc : service_config_) {
    LaunchService copy;
    ZX_ASSERT(svc.Clone(&copy) == ZX_OK);
    services->AddServiceWithLaunchInfo(
        svc.url,
        [this, svc = std::move(copy)]() {
          fuchsia::sys::LaunchInfo linfo;
          linfo.url = svc.url;
          linfo.arguments->insert(linfo.arguments->begin(),
                                  svc.arguments->begin(), svc.arguments->end());
          linfo.out = loggers_->CreateLogger(svc.url, false);
          linfo.err = loggers_->CreateLogger(svc.url, true);
          loggers_->IncrementCounter();
          return linfo;
        },
        svc.name);
  }

  // save all handles for virtual devices
  for (auto& dev : options.devices) {
    virtual_devices_.AddEntry(dev.path, dev.device.Bind());
  }

  fuchsia::sys::EnvironmentOptions sub_options = {
      .kill_on_oom = true,
      .allow_parent_runners = false,
      .inherit_parent_services = false};

  // Nested environments without a name are not allowed, if empty name is
  // provided, replace it with a default value:
  if (options.name.empty()) {
    options.name = "netemul-env";
  }

  env_ = EnclosingEnvironment::Create(options.name, parent, std::move(services),
                                      sub_options);

  env_->SetRunningChangedCallback([this](bool running) {
    if (running && running_callback_) {
      running_callback_();
    }
  });

  launcher_ = std::make_unique<ManagedLauncher>(this);
}

zx::channel ManagedEnvironment::OpenVdevDirectory() {
  return virtual_devices_.OpenAsDirectory();
}

zx::channel ManagedEnvironment::OpenVdataDirectory() {
  if (!virtual_data_) {
    virtual_data_ = std::make_unique<VirtualData>();
  }
  return virtual_data_->GetDirectory();
}

void ManagedEnvironment::Bind(
    fidl::InterfaceRequest<ManagedEnvironment::FManagedEnvironment> req) {
  bindings_.AddBinding(this, std::move(req));
}

ManagedLoggerCollection& ManagedEnvironment::loggers() {
  ZX_ASSERT(loggers_);
  return *loggers_;
}

}  // namespace netemul
