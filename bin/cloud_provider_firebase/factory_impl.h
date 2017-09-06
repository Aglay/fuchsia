// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_CLOUD_PROVIDER_FIREBASE_FACTORY_IMPL_H_
#define APPS_LEDGER_CLOUD_PROVIDER_FIREBASE_FACTORY_IMPL_H_

#include "apps/ledger/cloud_provider_firebase/cloud_provider_impl.h"
#include "apps/ledger/cloud_provider_firebase/services/factory.fidl.h"
#include "apps/ledger/services/cloud_provider/cloud_provider.fidl.h"
#include "apps/ledger/src/callback/auto_cleanable.h"
#include "garnet/public/lib/ftl/functional/closure.h"
#include "garnet/public/lib/ftl/macros.h"

namespace cloud_provider_firebase {

class FactoryImpl : public Factory {
 public:
  explicit FactoryImpl(ftl::RefPtr<ftl::TaskRunner> main_runner);
  ~FactoryImpl() override;

  void set_on_empty(const ftl::Closure& on_empty) {
    providers_.set_on_empty(on_empty);
  }

 private:
  // Factory:
  void GetCloudProvider(
      ConfigPtr config,
      fidl::InterfaceHandle<modular::auth::TokenProvider> token_provider,
      fidl::InterfaceRequest<cloud_provider::CloudProvider> cloud_provider,
      const GetCloudProviderCallback& callback) override;

  ftl::RefPtr<ftl::TaskRunner> main_runner_;

  callback::AutoCleanableSet<CloudProviderImpl> providers_;

  ftl::Closure on_empty_;

  FTL_DISALLOW_COPY_AND_ASSIGN(FactoryImpl);
};

}  // namespace cloud_provider_firebase

#endif  // APPS_LEDGER_CLOUD_PRFIREBASE_FACTORY_IMPL_H_
