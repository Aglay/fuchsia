// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/e2e_sync/ledger_app_instance_factory_e2e.h"

#include <utility>

#include <lib/component/cpp/service_provider_impl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fsl/socket/strings.h>
#include <lib/fxl/files/scoped_temp_dir.h>
#include <lib/fxl/random/uuid.h>
#include <lib/fxl/strings/string_view.h>
#include <lib/svc/cpp/services.h>

#include "peridot/bin/cloud_provider_firestore/testing/cloud_provider_factory.h"
#include "peridot/bin/ledger/fidl/include/types.h"
#include "peridot/bin/ledger/fidl_helpers/bound_interface_set.h"
#include "peridot/bin/ledger/testing/ledger_app_instance_factory.h"
#include "peridot/bin/ledger/tests/e2e_sync/ledger_app_instance_factory_e2e.h"
#include "peridot/lib/convert/convert.h"
#include "peridot/lib/firebase_auth/testing/fake_token_provider.h"

namespace ledger {
namespace {
constexpr fxl::StringView kLedgerName = "AppTests";

class LedgerAppInstanceImpl final
    : public LedgerAppInstanceFactory::LedgerAppInstance {
 public:
  LedgerAppInstanceImpl(
      LoopController* loop_controller,
      ledger_internal::LedgerRepositoryFactoryPtr ledger_repository_factory,
      SyncParams sync_params, std::string user_id);

  void Init(fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
                repository_factory_request);

 private:
  cloud_provider::CloudProviderPtr MakeCloudProvider() override;

  std::unique_ptr<component::StartupContext> startup_context_;
  component::ServiceProviderImpl service_provider_impl_;
  cloud_provider_firestore::CloudProviderFactory cloud_provider_factory_;

  fuchsia::sys::ComponentControllerPtr controller_;
  const std::string user_id_;
};

LedgerAppInstanceImpl::LedgerAppInstanceImpl(
    LoopController* loop_controller,
    ledger_internal::LedgerRepositoryFactoryPtr ledger_repository_factory,
    SyncParams sync_params, std::string user_id)
    : LedgerAppInstanceFactory::LedgerAppInstance(
          loop_controller, convert::ToArray(kLedgerName),
          std::move(ledger_repository_factory)),
      startup_context_(
          component::StartupContext::CreateFromStartupInfoNotChecked()),
      cloud_provider_factory_(startup_context_.get(),
                              std::move(sync_params.api_key),
                              std::move(sync_params.credentials)),
      user_id_(std::move(user_id)) {
  service_provider_impl_.AddService<fuchsia::modular::auth::TokenProvider>(
      [this](fidl::InterfaceRequest<fuchsia::modular::auth::TokenProvider>
                 request) {
        cloud_provider_factory_.MakeTokenProviderWithGivenUserId(
            user_id_, std::move(request));
      });
}

void LedgerAppInstanceImpl::Init(
    fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
        repository_factory_request) {
  cloud_provider_factory_.Init();

  component::Services child_services;
  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = "ledger";
  launch_info.directory_request = child_services.NewRequest();
  launch_info.arguments.push_back("--disable_reporting");
  fuchsia::sys::ServiceList service_list;
  service_list.names.push_back(fuchsia::modular::auth::TokenProvider::Name_);
  service_provider_impl_.AddBinding(service_list.provider.NewRequest());
  launch_info.additional_services = fidl::MakeOptional(std::move(service_list));

  startup_context_->launcher()->CreateComponent(std::move(launch_info),
                                                controller_.NewRequest());
  child_services.ConnectToService(std::move(repository_factory_request));
}

cloud_provider::CloudProviderPtr LedgerAppInstanceImpl::MakeCloudProvider() {
  cloud_provider::CloudProviderPtr cloud_provider;
  cloud_provider_factory_.MakeCloudProviderWithGivenUserId(
      user_id_, cloud_provider.NewRequest());
  return cloud_provider;
}

}  // namespace

LedgerAppInstanceFactoryImpl::LedgerAppInstanceFactoryImpl(
    SyncParams sync_params)
    : sync_params_(std::move(sync_params)),
      user_id_("e2e_test_" + fxl::GenerateUUID()) {}

LedgerAppInstanceFactoryImpl::~LedgerAppInstanceFactoryImpl() {}

std::unique_ptr<LedgerAppInstanceFactory::LedgerAppInstance>
LedgerAppInstanceFactoryImpl::NewLedgerAppInstance(
    LoopController* loop_controller) {
  ledger_internal::LedgerRepositoryFactoryPtr repository_factory;
  fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
      repository_factory_request = repository_factory.NewRequest();
  auto result = std::make_unique<LedgerAppInstanceImpl>(
      loop_controller, std::move(repository_factory), sync_params_, user_id_);
  result->Init(std::move(repository_factory_request));
  return result;
}

}  // namespace ledger
