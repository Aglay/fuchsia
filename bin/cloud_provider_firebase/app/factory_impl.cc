// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firebase/app/factory_impl.h"

#include "garnet/lib/backoff/exponential_backoff.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"

namespace cloud_provider_firebase {

FactoryImpl::FactoryImpl(async_t* async,
                         network_wrapper::NetworkWrapper* network_wrapper)
    : async_(async), network_wrapper_(network_wrapper) {}

FactoryImpl::~FactoryImpl() {}

void FactoryImpl::GetCloudProvider(
    Config config,
    fidl::InterfaceHandle<modular_auth::TokenProvider> token_provider,
    fidl::InterfaceRequest<cloud_provider::CloudProvider> cloud_provider,
    GetCloudProviderCallback callback) {
  auto token_provider_ptr = token_provider.Bind();
  auto firebase_auth = std::make_unique<firebase_auth::FirebaseAuthImpl>(
      async_, config.api_key, std::move(token_provider_ptr),
      std::make_unique<backoff::ExponentialBackoff>());
  firebase_auth::FirebaseAuthImpl* firebase_auth_ptr = firebase_auth.get();
  auto request = firebase_auth_ptr->GetFirebaseUserId(fxl::MakeCopyable([
    this, config = std::move(config), firebase_auth = std::move(firebase_auth),
    cloud_provider = std::move(cloud_provider), callback
  ](firebase_auth::AuthStatus status, std::string user_id) mutable {
    if (status != firebase_auth::AuthStatus::OK) {
      FXL_LOG(ERROR)
          << "Failed to retrieve the user ID from auth token provider";
      callback(cloud_provider::Status::AUTH_ERROR);
      return;
    }

    providers_.emplace(network_wrapper_, user_id,
                       std::move(config), std::move(firebase_auth),
                       std::move(cloud_provider));
    callback(cloud_provider::Status::OK);
  }));
  token_requests_.emplace(request);
}

}  // namespace cloud_provider_firebase
