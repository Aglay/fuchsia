// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTING_CLOUD_PROVIDER_FAKE_DEVICE_SET_H_
#define PERIDOT_BIN_LEDGER_TESTING_CLOUD_PROVIDER_FAKE_DEVICE_SET_H_

#include <set>
#include <string>

#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
#include "lib/fidl/cpp/bindings/array.h"
#include "lib/fxl/functional/closure.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/testing/cloud_provider/types.h"

namespace ledger {

class FakeDeviceSet : public cloud_provider::DeviceSet {
 public:
  FakeDeviceSet(CloudEraseOnCheck cloud_erase_on_check,
                CloudEraseFromWatcher cloud_erase_from_watcher);
  ~FakeDeviceSet() override;

  void set_on_empty(const fxl::Closure& on_empty) { on_empty_ = on_empty; }

 private:
  void CheckFingerprint(f1dl::Array<uint8_t> fingerprint,
                        const CheckFingerprintCallback& callback) override;

  void SetFingerprint(f1dl::Array<uint8_t> fingerprint,
                      const SetFingerprintCallback& callback) override;

  void SetWatcher(
      f1dl::Array<uint8_t> fingerprint,
      f1dl::InterfaceHandle<cloud_provider::DeviceSetWatcher> watcher,
      const SetWatcherCallback& callback) override;

  void Erase(const EraseCallback& callback) override;

  const CloudEraseOnCheck cloud_erase_on_check_ = CloudEraseOnCheck::NO;

  const CloudEraseFromWatcher cloud_erase_from_watcher_ =
      CloudEraseFromWatcher::NO;

  fxl::Closure on_empty_;

  std::set<std::string> fingerprints_;

  // Watcher set by the client.
  cloud_provider::DeviceSetWatcherPtr watcher_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakeDeviceSet);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTING_CLOUD_PROVIDER_FAKE_DEVICE_SET_H_
