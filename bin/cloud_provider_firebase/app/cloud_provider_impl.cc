// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firebase/app/cloud_provider_impl.h"

#include <utility>

#include "lib/fxl/logging.h"
#include "peridot/bin/cloud_provider_firebase/app/convert_status.h"
#include "peridot/bin/cloud_provider_firebase/device_set/cloud_device_set_impl.h"
#include "peridot/bin/cloud_provider_firebase/gcs/cloud_storage_impl.h"
#include "peridot/bin/cloud_provider_firebase/page_handler/impl/page_cloud_handler_impl.h"
#include "peridot/bin/cloud_provider_firebase/page_handler/impl/paths.h"
#include "peridot/lib/convert/convert.h"

namespace cloud_provider_firebase {

CloudProviderImpl::CloudProviderImpl(
    fxl::RefPtr<fxl::TaskRunner> main_runner,
    ledger::NetworkService* network_service,
    std::string user_id,
    ConfigPtr config,
    std::unique_ptr<firebase_auth::FirebaseAuth> firebase_auth,
    f1dl::InterfaceRequest<cloud_provider::CloudProvider> request)
    : main_runner_(std::move(main_runner)),
      network_service_(network_service),
      user_id_(std::move(user_id)),
      server_id_(config->server_id),
      firebase_auth_(std::move(firebase_auth)),
      binding_(this, std::move(request)) {
  // The class shuts down when the client connection is disconnected.
  binding_.set_error_handler([this] {
    if (on_empty_) {
      on_empty_();
    }
  });
  // The class also shuts down when the auth provider is disconnected.
  firebase_auth_->set_error_handler([this] {
    FXL_LOG(ERROR) << "Lost connection to the token provider, "
                   << "shutting down the cloud provider.";
    if (on_empty_) {
      on_empty_();
    }
  });
}

CloudProviderImpl::~CloudProviderImpl() {}

void CloudProviderImpl::GetDeviceSet(
    f1dl::InterfaceRequest<cloud_provider::DeviceSet> device_set,
    const GetDeviceSetCallback& callback) {
  auto user_firebase = std::make_unique<firebase::FirebaseImpl>(
      network_service_, server_id_, GetFirebasePathForUser(user_id_));
  auto cloud_device_set =
      std::make_unique<CloudDeviceSetImpl>(std::move(user_firebase));
  device_sets_.emplace(firebase_auth_.get(), std::move(cloud_device_set),
                       std::move(device_set));
  callback(cloud_provider::Status::OK);
}

void CloudProviderImpl::GetPageCloud(
    f1dl::Array<uint8_t> app_id,
    f1dl::Array<uint8_t> page_id,
    f1dl::InterfaceRequest<cloud_provider::PageCloud> page_cloud,
    const GetPageCloudCallback& callback) {
  std::string app_id_str = convert::ToString(app_id);
  std::string page_id_str = convert::ToString(page_id);

  std::string app_firebase_path = GetFirebasePathForApp(user_id_, app_id_str);
  auto firebase = std::make_unique<firebase::FirebaseImpl>(
      network_service_, server_id_,
      GetFirebasePathForPage(app_firebase_path, page_id_str));

  std::string app_gcs_prefix = GetGcsPrefixForApp(user_id_, app_id_str);
  auto cloud_storage = std::make_unique<gcs::CloudStorageImpl>(
      main_runner_, network_service_, server_id_,
      GetGcsPrefixForPage(app_gcs_prefix, page_id_str));

  auto handler =
      std::make_unique<cloud_provider_firebase::PageCloudHandlerImpl>(
          firebase.get(), cloud_storage.get());
  page_clouds_.emplace(firebase_auth_.get(), std::move(firebase),
                       std::move(cloud_storage), std::move(handler),
                       std::move(page_cloud));
  callback(cloud_provider::Status::OK);
}

}  // namespace cloud_provider_firebase
