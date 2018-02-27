// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_FACTORY_IMPL_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_FACTORY_IMPL_H_

#include "garnet/lib/callback/auto_cleanable.h"
#include "garnet/lib/callback/cancellable.h"
#include "lib/auth/fidl/token_provider.fidl.h"
#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/memory/ref_ptr.h"
#include "lib/fxl/tasks/task_runner.h"
#include "peridot/bin/cloud_provider_firestore/app/cloud_provider_impl.h"
#include "peridot/bin/cloud_provider_firestore/fidl/factory.fidl.h"

namespace cloud_provider_firestore {

class FactoryImpl : public Factory {
 public:
  explicit FactoryImpl(fxl::RefPtr<fxl::TaskRunner> main_runner);

  ~FactoryImpl() override;

  // Shuts down all cloud providers owned by the class.
  //
  // It is only valid to delete the factory after the completion callback is
  // called.
  void ShutDown(fxl::Closure callback);

 private:
  // Factory:
  void GetCloudProvider(
      ConfigPtr config,
      f1dl::InterfaceHandle<modular::auth::TokenProvider> token_provider,
      f1dl::InterfaceRequest<cloud_provider::CloudProvider>
          cloud_provider_request,
      const GetCloudProviderCallback& callback) override;

  fxl::RefPtr<fxl::TaskRunner> main_runner_;
  callback::CancellableContainer token_requests_;
  callback::AutoCleanableSet<CloudProviderImpl> providers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FactoryImpl);
};

}  // namespace cloud_provider_firestore

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_FACTORY_IMPL_H_
