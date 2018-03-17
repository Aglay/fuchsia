// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/environment_host/maxwell_service_provider_bridge.h"

#include "lib/app/cpp/connect.h"
#include "lib/app/fidl/application_loader.fidl.h"

namespace maxwell {
namespace {

// TODO(abarth): Get this constant from a generated header once netstack uses
// FIDL.
constexpr char kNetstack[] = "net.Netstack";

}  // namespace

MaxwellServiceProviderBridge::MaxwellServiceProviderBridge(
    component::ApplicationEnvironment* parent_env) {
  AddService<component::ApplicationLoader>(
      [parent_env](
          f1dl::InterfaceRequest<component::ApplicationLoader> request) {
        component::ServiceProviderPtr services;
        parent_env->GetServices(services.NewRequest());
        component::ConnectToService(services.get(), std::move(request));
      });
  AddServiceForName(
      [parent_env](zx::channel request) {
        component::ServiceProviderPtr services;
        parent_env->GetServices(services.NewRequest());
        services->ConnectToService(kNetstack, std::move(request));
      },
      kNetstack);
}

}  // namespace maxwell
