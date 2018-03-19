// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_DEVICE_SET_H_
#define PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_DEVICE_SET_H_

#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
#include "lib/fidl/cpp/bindings/array.h"
#include "lib/fxl/macros.h"

namespace cloud_sync {

class TestDeviceSet : public cloud_provider::DeviceSet {
 public:
  TestDeviceSet();
  ~TestDeviceSet() override;

  cloud_provider::Status status_to_return = cloud_provider::Status::OK;
  cloud_provider::Status set_watcher_status_to_return =
      cloud_provider::Status::OK;
  std::string checked_fingerprint;
  std::string set_fingerprint;

  int set_watcher_calls = 0;
  std::string watched_fingerprint;
  cloud_provider::DeviceSetWatcherPtr set_watcher;

 private:
  // cloud_provider::DeviceSet:
  void CheckFingerprint(f1dl::VectorPtr<uint8_t> fingerprint,
                        const CheckFingerprintCallback& callback) override;

  void SetFingerprint(f1dl::VectorPtr<uint8_t> fingerprint,
                      const SetFingerprintCallback& callback) override;

  void SetWatcher(
      f1dl::VectorPtr<uint8_t> fingerprint,
      f1dl::InterfaceHandle<cloud_provider::DeviceSetWatcher> watcher,
      const SetWatcherCallback& callback) override;

  void Erase(const EraseCallback& callback) override;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestDeviceSet);
};

}  // namespace cloud_sync

#endif  // PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_DEVICE_SET_H_
