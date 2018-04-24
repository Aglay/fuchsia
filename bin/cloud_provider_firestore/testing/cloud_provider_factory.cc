// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firestore/testing/cloud_provider_factory.h"

#include <utility>

#include <fuchsia/cpp/network.h>
#include <lib/async/cpp/task.h>

#include "garnet/lib/backoff/exponential_backoff.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/random/uuid.h"
#include "lib/svc/cpp/services.h"

namespace cloud_provider_firestore {
namespace {
constexpr char kAppUrl[] = "cloud_provider_firestore";
}  // namespace

class CloudProviderFactory::TokenProviderContainer {
 public:
  TokenProviderContainer(
      component::ApplicationContext* application_context,
      async_t* async,
      std::string credentials_path,
      fidl::InterfaceRequest<modular_auth::TokenProvider> request)
      : application_context_(application_context),
        network_wrapper_(
            async,
            std::make_unique<backoff::ExponentialBackoff>(),
            [this] {
              return application_context_
                  ->ConnectToEnvironmentService<network::NetworkService>();
            }),
        token_provider_(&network_wrapper_, fxl::GenerateUUID()),
        binding_(&token_provider_, std::move(request)) {
    if (!token_provider_.LoadCredentials(credentials_path)) {
      FXL_LOG(ERROR) << "Failed to load token provider credentials at: "
                     << credentials_path;
    }
  }

  void set_on_empty(fxl::Closure on_empty) {
    binding_.set_error_handler(std::move(on_empty));
  }

 private:
  component::ApplicationContext* const application_context_;
  network_wrapper::NetworkWrapperImpl network_wrapper_;
  service_account::ServiceAccountTokenProvider token_provider_;
  fidl::Binding<modular_auth::TokenProvider> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TokenProviderContainer);
};

CloudProviderFactory::CloudProviderFactory(
    component::ApplicationContext* application_context,
    std::string credentials_path)
    : application_context_(application_context),
      credentials_path_(std::move(credentials_path)) {}

CloudProviderFactory::~CloudProviderFactory() {
  services_loop_.Shutdown();
}

void CloudProviderFactory::Init() {
  services_loop_.StartThread();
  component::Services child_services;
  component::ApplicationLaunchInfo launch_info;
  launch_info.url = kAppUrl;
  launch_info.directory_request = child_services.NewRequest();
  application_context_->launcher()->CreateApplication(
      std::move(launch_info), cloud_provider_controller_.NewRequest());
  child_services.ConnectToService(cloud_provider_factory_.NewRequest());
}

void CloudProviderFactory::MakeCloudProvider(
    std::string server_id,
    std::string api_key,
    fidl::InterfaceRequest<cloud_provider::CloudProvider> request) {
  if (api_key.empty()) {
    FXL_LOG(WARNING) << "Empty Firebase API key - this can possibly work "
                     << "only with unauthenticated server instances.";
  }
  modular_auth::TokenProviderPtr token_provider;
  async::PostTask(services_loop_.async(), fxl::MakeCopyable(
      [ this, request = token_provider.NewRequest() ]() mutable {
        token_providers_.emplace(application_context_, services_loop_.async(),
                                 credentials_path_, std::move(request));
      }));

  cloud_provider_firestore::Config firebase_config;
  firebase_config.server_id = server_id;
  firebase_config.api_key = api_key;

  cloud_provider_factory_->GetCloudProvider(
      std::move(firebase_config), std::move(token_provider), std::move(request),
      [](cloud_provider::Status status) {
        if (status != cloud_provider::Status::OK) {
          FXL_LOG(ERROR) << "Failed to create a cloud provider: " << status;
        }
      });
}

}  // namespace cloud_provider_firestore
