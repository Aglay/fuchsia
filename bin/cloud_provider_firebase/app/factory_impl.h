// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_APP_FACTORY_IMPL_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_APP_FACTORY_IMPL_H_

#include <cloud_provider/cpp/fidl.h>
#include <cloud_provider_firebase/cpp/fidl.h>
#include <modular_auth/cpp/fidl.h>
#include <lib/async/dispatcher.h>

#include "lib/callback/auto_cleanable.h"
#include "lib/callback/cancellable.h"
#include "lib/fxl/functional/closure.h"
#include "lib/fxl/macros.h"
#include "lib/network_wrapper/network_wrapper.h"
#include "peridot/bin/cloud_provider_firebase/app/cloud_provider_impl.h"

namespace cloud_provider_firebase {

class FactoryImpl : public Factory {
 public:
  explicit FactoryImpl(async_t* async,
                       network_wrapper::NetworkWrapper* network_wrapper);

  ~FactoryImpl() override;

 private:
  // Factory:
  void GetCloudProvider(
      Config config,
      fidl::InterfaceHandle<modular_auth::TokenProvider> token_provider,
      fidl::InterfaceRequest<cloud_provider::CloudProvider> cloud_provider,
      GetCloudProviderCallback callback) override;

  async_t* const async_;
  network_wrapper::NetworkWrapper* const network_wrapper_;
  callback::CancellableContainer token_requests_;
  callback::AutoCleanableSet<CloudProviderImpl> providers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FactoryImpl);
};

}  // namespace cloud_provider_firebase

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_APP_FACTORY_IMPL_H_
