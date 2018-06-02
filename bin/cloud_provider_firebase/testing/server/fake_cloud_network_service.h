// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_FAKE_CLOUD_NETWORK_SERVICE_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_FAKE_CLOUD_NETWORK_SERVICE_H_

#include <fuchsia/netstack/cpp/fidl.h>
#include <network/cpp/fidl.h>
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/cloud_provider_firebase/testing/server/fake_cloud_url_loader.h"

namespace ledger {

// Implementation of network::NetworkService that simulates Firebase and GCS
// servers.
class FakeCloudNetworkService : public network::NetworkService {
 public:
  FakeCloudNetworkService();
  ~FakeCloudNetworkService() override;

  // network::NetworkService
  void CreateURLLoader(
      ::fidl::InterfaceRequest<network::URLLoader> loader) override;
  void GetCookieStore(zx::channel cookie_store) override;
  void CreateWebSocket(zx::channel socket) override;
  // Bind a new request to this implementation.
  void AddBinding(fidl::InterfaceRequest<network::NetworkService> request);

 private:
  FakeCloudURLLoader url_loader_;
  fidl::BindingSet<network::URLLoader> loader_bindings_;
  fidl::BindingSet<network::NetworkService> bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakeCloudNetworkService);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_FAKE_CLOUD_NETWORK_SERVICE_H_
