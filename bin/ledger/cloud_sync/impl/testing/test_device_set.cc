// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/cloud_sync/impl/testing/test_device_set.h"

#include "peridot/lib/convert/convert.h"

namespace cloud_sync {

TestDeviceSet::TestDeviceSet() {}
TestDeviceSet::~TestDeviceSet() {}

void TestDeviceSet::CheckFingerprint(f1dl::VectorPtr<uint8_t> fingerprint,
                                     const CheckFingerprintCallback& callback) {
  checked_fingerprint = convert::ToString(fingerprint);
  callback(status_to_return);
}

void TestDeviceSet::SetFingerprint(f1dl::VectorPtr<uint8_t> fingerprint,
                                   const SetFingerprintCallback& callback) {
  set_fingerprint = convert::ToString(fingerprint);
  callback(status_to_return);
}

void TestDeviceSet::SetWatcher(
    f1dl::VectorPtr<uint8_t> fingerprint,
    f1dl::InterfaceHandle<cloud_provider::DeviceSetWatcher> watcher,
    const SetWatcherCallback& callback) {
  set_watcher_calls++;
  watched_fingerprint = convert::ToString(fingerprint);
  set_watcher = watcher.Bind();
  if (set_watcher_status_to_return == cloud_provider::Status::NETWORK_ERROR) {
    set_watcher->OnNetworkError();
  }
  callback(set_watcher_status_to_return);
}

void TestDeviceSet::Erase(const EraseCallback& callback) {
  callback(status_to_return);
}

}  // namespace cloud_sync
