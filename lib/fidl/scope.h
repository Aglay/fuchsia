// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_FIDL_SCOPE_H_
#define PERIDOT_LIB_FIDL_SCOPE_H_

#include <memory>
#include <string>

#include <component/cpp/fidl.h>
#include "lib/app/cpp/service_provider_impl.h"
#include "lib/app/cpp/startup_context.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/svc/cpp/service_provider_bridge.h"

namespace fuchsia {
namespace modular {

// Provides fate separation of sets of applications run by one application. The
// environment services are delegated to the parent environment.
class Scope {
 public:
  Scope(const component::EnvironmentPtr& parent_env, const std::string& label);

  Scope(const Scope* parent_scope, const std::string& label);

  template <typename Interface>
  void AddService(
      component::ServiceProviderImpl::InterfaceRequestHandler<Interface>
          handler,
      const std::string& service_name = Interface::Name_) {
    service_provider_bridge_.AddService(handler, service_name);
  }

  component::ApplicationLauncher* GetLauncher();

  const component::EnvironmentPtr& environment() const { return env_; }

 private:
  void InitScope(const component::EnvironmentPtr& parent_env,
                 const std::string& label);

  component::ServiceProviderBridge service_provider_bridge_;
  component::EnvironmentPtr env_;
  component::ApplicationLauncherPtr env_launcher_;
  component::EnvironmentControllerPtr env_controller_;
};

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_LIB_FIDL_SCOPE_H_
