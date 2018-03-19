// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/testing/cloud_provider/fake_cloud_provider.h"

#include "peridot/lib/convert/convert.h"

namespace ledger {

FakeCloudProvider::FakeCloudProvider(
    CloudEraseOnCheck cloud_erase_on_check,
    CloudEraseFromWatcher cloud_erase_from_watcher)
    : device_set_(cloud_erase_on_check, cloud_erase_from_watcher) {}

FakeCloudProvider::~FakeCloudProvider() {}

void FakeCloudProvider::GetDeviceSet(
    f1dl::InterfaceRequest<cloud_provider::DeviceSet> device_set,
    const GetDeviceSetCallback& callback) {
  device_set_.AddBinding(std::move(device_set));
  callback(cloud_provider::Status::OK);
}

void FakeCloudProvider::GetPageCloud(
    f1dl::VectorPtr<uint8_t> app_id,
    f1dl::VectorPtr<uint8_t> page_id,
    f1dl::InterfaceRequest<cloud_provider::PageCloud> page_cloud,
    const GetPageCloudCallback& callback) {
  const std::string key =
      convert::ToString(app_id) + "_" + convert::ToString(page_id);
  auto it = page_clouds_.find(key);
  if (it != page_clouds_.end()) {
    it->second.Bind(std::move(page_cloud));
    callback(cloud_provider::Status::OK);
    return;
  }

  auto ret =
      page_clouds_.emplace(std::piecewise_construct, std::forward_as_tuple(key),
                           std::forward_as_tuple());
  ret.first->second.Bind(std::move(page_cloud));
  callback(cloud_provider::Status::OK);
}

}  // namespace ledger
