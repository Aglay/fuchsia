// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_APP_CLOUD_PROVIDER_IMPL_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_APP_CLOUD_PROVIDER_IMPL_H_

#include "garnet/lib/callback/auto_cleanable.h"
#include "garnet/lib/callback/cancellable.h"
#include <fuchsia/cpp/modular_auth.h>
#include <fuchsia/cpp/cloud_provider.h>
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/functional/closure.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/tasks/task_runner.h"
#include "peridot/bin/cloud_provider_firebase/app/device_set_impl.h"
#include "peridot/bin/cloud_provider_firebase/app/page_cloud_impl.h"
#include "peridot/bin/cloud_provider_firebase/fidl/factory.fidl.h"
#include "peridot/lib/firebase/firebase_impl.h"
#include "peridot/lib/firebase_auth/firebase_auth_impl.h"
#include "garnet/lib/network_wrapper/network_wrapper.h"

namespace cloud_provider_firebase {

// Implementation of cloud_provider::CloudProvider.
//
// If the |on_empty| callback is set, it is called when the client connection is
// closed.
class CloudProviderImpl : public cloud_provider::CloudProvider {
 public:
  CloudProviderImpl(
      fxl::RefPtr<fxl::TaskRunner> main_runner,
      network_wrapper::NetworkWrapper* network_wrapper,
      std::string user_id,
      ConfigPtr config,
      std::unique_ptr<firebase_auth::FirebaseAuth> firebase_auth,
      f1dl::InterfaceRequest<cloud_provider::CloudProvider> request);
  ~CloudProviderImpl() override;

  void set_on_empty(const fxl::Closure& on_empty) { on_empty_ = on_empty; }

 private:
  void GetDeviceSet(
      f1dl::InterfaceRequest<cloud_provider::DeviceSet> device_set,
      const GetDeviceSetCallback& callback) override;

  void GetPageCloud(
      f1dl::VectorPtr<uint8_t> app_id,
      f1dl::VectorPtr<uint8_t> page_id,
      f1dl::InterfaceRequest<cloud_provider::PageCloud> page_cloud,
      const GetPageCloudCallback& callback) override;

  fxl::RefPtr<fxl::TaskRunner> main_runner_;
  network_wrapper::NetworkWrapper* const network_wrapper_;
  const std::string user_id_;
  const std::string server_id_;
  std::unique_ptr<firebase_auth::FirebaseAuth> firebase_auth_;
  f1dl::Binding<cloud_provider::CloudProvider> binding_;
  fxl::Closure on_empty_;

  callback::AutoCleanableSet<DeviceSetImpl> device_sets_;

  callback::AutoCleanableSet<PageCloudImpl> page_clouds_;

  // Pending auth token requests to be cancelled when this class goes away.
  callback::CancellableContainer auth_token_requests_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CloudProviderImpl);
};

}  // namespace cloud_provider_firebase

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_APP_CLOUD_PROVIDER_IMPL_H_
