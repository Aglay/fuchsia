// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_FAKE_CLOUD_URL_LOADER_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_FAKE_CLOUD_URL_LOADER_H_

#include <map>

#include "lib/fxl/macros.h"
#include <fuchsia/cpp/network.h>
#include "peridot/bin/cloud_provider_firebase/testing/server/firebase_server.h"
#include "peridot/bin/cloud_provider_firebase/testing/server/gcs_server.h"

namespace ledger {

// Implementation of |URLLoader| that simulate Firebase and GCS
// servers.
class FakeCloudURLLoader : public network::URLLoader {
 public:
  FakeCloudURLLoader();
  ~FakeCloudURLLoader() override;

  // URLLoader
  void Start(network::URLRequestPtr request,
             const StartCallback& callback) override;
  void FollowRedirect(const FollowRedirectCallback& callback) override;
  void QueryStatus(const QueryStatusCallback& callback) override;

 private:
  std::map<std::string, FirebaseServer> firebase_servers_;
  std::map<std::string, GcsServer> gcs_servers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakeCloudURLLoader);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_FAKE_CLOUD_URL_LOADER_H_
