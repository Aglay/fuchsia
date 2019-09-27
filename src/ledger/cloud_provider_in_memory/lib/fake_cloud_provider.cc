// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/cloud_provider_in_memory/lib/fake_cloud_provider.h"

#include "peridot/lib/convert/convert.h"

namespace ledger {

FakeCloudProvider::Builder::Builder(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

FakeCloudProvider::Builder::~Builder() = default;

FakeCloudProvider::Builder& FakeCloudProvider::Builder::SetInjectNetworkError(
    InjectNetworkError inject_network_error) {
  inject_network_error_ = inject_network_error;
  return *this;
}

FakeCloudProvider::Builder& FakeCloudProvider::Builder::SetCloudEraseOnCheck(
    CloudEraseOnCheck cloud_erase_on_check) {
  cloud_erase_on_check_ = cloud_erase_on_check;
  return *this;
}

FakeCloudProvider::Builder& FakeCloudProvider::Builder::SetCloudEraseFromWatcher(
    CloudEraseFromWatcher cloud_erase_from_watcher) {
  cloud_erase_from_watcher_ = cloud_erase_from_watcher;
  return *this;
}

std::unique_ptr<FakeCloudProvider> FakeCloudProvider::Builder::Build() {
  return std::make_unique<FakeCloudProvider>(*this);
}

FakeCloudProvider::FakeCloudProvider(const Builder& builder)
    : dispatcher_(builder.dispatcher_),
      device_set_(builder.cloud_erase_on_check_, builder.cloud_erase_from_watcher_),
      page_clouds_(builder.dispatcher_),
      inject_network_error_(builder.inject_network_error_) {}

FakeCloudProvider::FakeCloudProvider(async_dispatcher_t* dispatcher)
    : FakeCloudProvider(Builder(dispatcher)) {}

FakeCloudProvider::~FakeCloudProvider() = default;

void FakeCloudProvider::GetDeviceSet(fidl::InterfaceRequest<cloud_provider::DeviceSet> device_set,
                                     GetDeviceSetCallback callback) {
  device_set_.AddBinding(std::move(device_set));
  callback(cloud_provider::Status::OK);
}

void FakeCloudProvider::GetPageCloud(std::vector<uint8_t> app_id, std::vector<uint8_t> page_id,
                                     fidl::InterfaceRequest<cloud_provider::PageCloud> page_cloud,
                                     GetPageCloudCallback callback) {
  const std::string key = convert::ToString(app_id) + "_" + convert::ToString(page_id);
  auto it = page_clouds_.find(key);
  if (it != page_clouds_.end()) {
    it->second.Bind(std::move(page_cloud));
    callback(cloud_provider::Status::OK);
    return;
  }

  auto ret = page_clouds_.emplace(std::piecewise_construct, std::forward_as_tuple(key),
                                  std::forward_as_tuple(dispatcher_, inject_network_error_));
  ret.first->second.Bind(std::move(page_cloud));
  callback(cloud_provider::Status::OK);
}

}  // namespace ledger
