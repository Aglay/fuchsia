// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_FACTORY_IMPL_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_FACTORY_IMPL_H_

#include <cloud_provider/cpp/fidl.h>
#include <cloud_provider_firestore/cpp/fidl.h>
#include <modular_auth/cpp/fidl.h>
#include <lib/async/dispatcher.h>

#include "lib/callback/auto_cleanable.h"
#include "lib/callback/cancellable.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/memory/ref_ptr.h"
#include "peridot/bin/cloud_provider_firestore/app/cloud_provider_impl.h"

namespace cloud_provider_firestore {

class FactoryImpl : public Factory {
 public:
  explicit FactoryImpl(async_t* async);

  ~FactoryImpl() override;

  // Shuts down all cloud providers owned by the class.
  //
  // It is only valid to delete the factory after the completion callback is
  // called.
  void ShutDown(fxl::Closure callback);

 private:
  // Factory:
  void GetCloudProvider(
      Config config,
      fidl::InterfaceHandle<modular_auth::TokenProvider> token_provider,
      fidl::InterfaceRequest<cloud_provider::CloudProvider>
          cloud_provider_request,
      GetCloudProviderCallback callback) override;

  async_t* const async_;
  callback::CancellableContainer token_requests_;
  callback::AutoCleanableSet<CloudProviderImpl> providers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FactoryImpl);
};

}  // namespace cloud_provider_firestore

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_FACTORY_IMPL_H_
