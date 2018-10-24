// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_ENVIRONMENT_HOST_MAXWELL_SERVICE_PROVIDER_BRIDGE_H_
#define PERIDOT_LIB_ENVIRONMENT_HOST_MAXWELL_SERVICE_PROVIDER_BRIDGE_H_

#include <fbl/ref_ptr.h>
#include <fs/pseudo-dir.h>
#include <fs/service.h>
#include <fs/synchronous-vfs.h>
#include <fuchsia/sys/cpp/fidl.h>

namespace maxwell {

// Environment surfacing only explicitly given environment services.
class MaxwellServiceProviderBridge {
 public:
  MaxwellServiceProviderBridge(fuchsia::sys::Environment* parent_env);

  zx::channel OpenAsDirectory();
  const fidl::VectorPtr<fidl::StringPtr>& service_names() {
    return service_names_;
  }

  template <typename Interface>
  void AddService(fidl::InterfaceRequestHandler<Interface> handler) {
    auto service = fbl::AdoptRef(new fs::Service(
        [this, handler = std::move(handler)](zx::channel channel) {
          handler(fidl::InterfaceRequest<Interface>(std::move(channel)));
          return ZX_OK;
        }));
    AddServiceWithName(Interface::Name_, service);
  }

  void AddServiceWithName(const char* name, fbl::RefPtr<fs::Service> svc);

 private:
  fs::SynchronousVfs vfs_;
  fbl::RefPtr<fs::PseudoDir> services_dir_;
  fidl::VectorPtr<fidl::StringPtr> service_names_;
};

}  // namespace maxwell

#endif  // PERIDOT_LIB_ENVIRONMENT_HOST_MAXWELL_SERVICE_PROVIDER_BRIDGE_H_
