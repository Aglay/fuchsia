// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_DEVICE_SET_TESTING_TEST_CLOUD_DEVICE_SET_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_DEVICE_SET_TESTING_TEST_CLOUD_DEVICE_SET_H_

#include <functional>
#include <string>

#include <lib/async/dispatcher.h>
#include <lib/fit/function.h>

#include "peridot/bin/cloud_provider_firebase/device_set/cloud_device_set.h"

namespace cloud_provider_firebase {

class TestCloudDeviceSet : public cloud_provider_firebase::CloudDeviceSet {
 public:
  explicit TestCloudDeviceSet(async_dispatcher_t* dispatcher);

  ~TestCloudDeviceSet() override;

  void CheckFingerprint(std::string auth_token, std::string fingerprint,
                        fit::function<void(Status)> callback) override;

  void SetFingerprint(std::string auth_token, std::string fingerprint,
                      fit::function<void(Status)> callback) override;

  void WatchFingerprint(std::string auth_token, std::string fingerprint,
                        fit::function<void(Status)> callback) override;

  void EraseAllFingerprints(std::string auth_token,
                            fit::function<void(Status)> callback) override;

  void UpdateTimestampAssociatedWithFingerprint(
      std::string auth_token, std::string fingerprint) override;

  CloudDeviceSet::Status status_to_return = CloudDeviceSet::Status::OK;

  std::string checked_fingerprint;
  std::string set_fingerprint;
  std::string watched_fingerprint;
  fit::function<void(Status)> watch_callback;
  int timestamp_update_requests_ = 0;

 private:
  async_dispatcher_t* const dispatcher_;
};

}  // namespace cloud_provider_firebase

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_DEVICE_SET_TESTING_TEST_CLOUD_DEVICE_SET_H_
