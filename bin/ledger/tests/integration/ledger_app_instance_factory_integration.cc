// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/backoff/exponential_backoff.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fit/function.h>
#include <lib/fsl/handles/object_info.h>
#include <lib/fsl/socket/strings.h>
#include <lib/fxl/files/scoped_temp_dir.h>

#include "gtest/gtest.h"
#include "peridot/bin/ledger/app/ledger_repository_factory_impl.h"
#include "peridot/bin/ledger/fidl_helpers/bound_interface_set.h"
#include "peridot/bin/ledger/p2p_provider/impl/p2p_provider_impl.h"
#include "peridot/bin/ledger/p2p_sync/impl/user_communicator_impl.h"
#include "peridot/bin/ledger/p2p_sync/public/user_communicator_factory.h"
#include "peridot/bin/ledger/testing/cloud_provider/fake_cloud_provider.h"
#include "peridot/bin/ledger/testing/cloud_provider/types.h"
#include "peridot/bin/ledger/testing/ledger_app_instance_factory.h"
#include "peridot/bin/ledger/testing/netconnector/netconnector_factory.h"
#include "peridot/bin/ledger/tests/integration/test_utils.h"
#include "peridot/lib/socket/socket_pair.h"
#include "peridot/lib/socket/socket_writer.h"

namespace ledger {
namespace {

constexpr zx::duration kBackoffDuration = zx::msec(5);
const char kUserId[] = "user";

Environment BuildEnvironment(async_dispatcher_t* dispatcher) {
  return EnvironmentBuilder()
      .SetAsync(dispatcher)
      // TODO(qsr) LE-558 Consider using a different dispatcher here.
      .SetIOAsync(dispatcher)
      .SetBackoffFactory([] {
        return std::make_unique<backoff::ExponentialBackoff>(
            kBackoffDuration, 1u, kBackoffDuration);
      })
      .Build();
}

class FakeUserIdProvider : public p2p_provider::UserIdProvider {
 public:
  FakeUserIdProvider() {}

  void GetUserId(fit::function<void(Status, std::string)> callback) override {
    callback(Status::OK, kUserId);
  };
};

class LedgerAppInstanceImpl final
    : public LedgerAppInstanceFactory::LedgerAppInstance {
 public:
  LedgerAppInstanceImpl(
      LedgerAppInstanceFactory::LoopController* loop_controller,
      async_dispatcher_t* services_dispatcher,
      fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
          repository_factory_request,
      fidl::InterfacePtr<ledger_internal::LedgerRepositoryFactory>
          repository_factory_ptr,
      fidl_helpers::BoundInterfaceSet<cloud_provider::CloudProvider,
                                      FakeCloudProvider>* cloud_provider,
      std::unique_ptr<p2p_sync::UserCommunicatorFactory>
          user_communicator_factory);
  ~LedgerAppInstanceImpl() override;

 private:
  class LedgerRepositoryFactoryContainer {
   public:
    LedgerRepositoryFactoryContainer(
        async_dispatcher_t* dispatcher,
        fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
            request,
        std::unique_ptr<p2p_sync::UserCommunicatorFactory>
            user_communicator_factory)
        : environment_(BuildEnvironment(dispatcher)),
          factory_impl_(&environment_, std::move(user_communicator_factory)),
          factory_binding_(&factory_impl_, std::move(request)) {}
    ~LedgerRepositoryFactoryContainer() {}

   private:
    Environment environment_;
    LedgerRepositoryFactoryImpl factory_impl_;
    fidl::Binding<ledger_internal::LedgerRepositoryFactory> factory_binding_;

    FXL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryFactoryContainer);
  };

  cloud_provider::CloudProviderPtr MakeCloudProvider() override;

  async::Loop loop_;
  std::unique_ptr<LedgerRepositoryFactoryContainer> factory_container_;
  async_dispatcher_t* services_dispatcher_;
  fidl_helpers::BoundInterfaceSet<cloud_provider::CloudProvider,
                                  FakeCloudProvider>* const cloud_provider_;

  // This must be the last field of this class.
  fxl::WeakPtrFactory<LedgerAppInstanceImpl> weak_ptr_factory_;
};

LedgerAppInstanceImpl::LedgerAppInstanceImpl(
    LedgerAppInstanceFactory::LoopController* loop_controller,
    async_dispatcher_t* services_dispatcher,
    fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
        repository_factory_request,
    fidl::InterfacePtr<ledger_internal::LedgerRepositoryFactory>
        repository_factory_ptr,
    fidl_helpers::BoundInterfaceSet<cloud_provider::CloudProvider,
                                    FakeCloudProvider>* cloud_provider,
    std::unique_ptr<p2p_sync::UserCommunicatorFactory>
        user_communicator_factory)
    : LedgerAppInstanceFactory::LedgerAppInstance(
          loop_controller, RandomArray(1), std::move(repository_factory_ptr)),
      loop_(&kAsyncLoopConfigNoAttachToThread),
      services_dispatcher_(services_dispatcher),
      cloud_provider_(cloud_provider),
      weak_ptr_factory_(this) {
  loop_.StartThread();
  async::PostTask(loop_.dispatcher(),
                  [this, request = std::move(repository_factory_request),
                   user_communicator_factory =
                       std::move(user_communicator_factory)]() mutable {
                    factory_container_ =
                        std::make_unique<LedgerRepositoryFactoryContainer>(
                            loop_.dispatcher(), std::move(request),
                            std::move(user_communicator_factory));
                  });
}

cloud_provider::CloudProviderPtr LedgerAppInstanceImpl::MakeCloudProvider() {
  cloud_provider::CloudProviderPtr cloud_provider;
  async::PostTask(services_dispatcher_,
                  callback::MakeScoped(
                      weak_ptr_factory_.GetWeakPtr(),
                      [this, request = cloud_provider.NewRequest()]() mutable {
                        cloud_provider_->AddBinding(std::move(request));
                      }));
  return cloud_provider;
}

LedgerAppInstanceImpl::~LedgerAppInstanceImpl() {
  async::PostTask(loop_.dispatcher(), [this] {
    factory_container_.reset();
    loop_.Quit();
  });
  loop_.JoinThreads();
}

class FakeUserCommunicatorFactory : public p2p_sync::UserCommunicatorFactory {
 public:
  FakeUserCommunicatorFactory(async_dispatcher_t* services_dispatcher,
                              NetConnectorFactory* netconnector_factory,
                              std::string host_name)
      : services_dispatcher_(services_dispatcher),
        environment_(BuildEnvironment(services_dispatcher)),
        netconnector_factory_(netconnector_factory),
        host_name_(std::move(host_name)),
        weak_ptr_factory_(this) {}
  ~FakeUserCommunicatorFactory() override {}

  std::unique_ptr<p2p_sync::UserCommunicator> GetUserCommunicator(
      DetachedPath /*user_directory*/) override {
    fuchsia::netconnector::NetConnectorPtr netconnector;
    async::PostTask(services_dispatcher_,
                    callback::MakeScoped(
                        weak_ptr_factory_.GetWeakPtr(),
                        [this, request = netconnector.NewRequest()]() mutable {
                          netconnector_factory_->AddBinding(host_name_,
                                                            std::move(request));
                        }));
    std::unique_ptr<p2p_provider::P2PProvider> provider =
        std::make_unique<p2p_provider::P2PProviderImpl>(
            host_name_, std::move(netconnector),
            std::make_unique<FakeUserIdProvider>());
    return std::make_unique<p2p_sync::UserCommunicatorImpl>(
        std::move(provider), environment_.coroutine_service());
  }

 private:
  async_dispatcher_t* const services_dispatcher_;
  Environment environment_;
  NetConnectorFactory* const netconnector_factory_;
  std::string host_name_;

  // This must be the last field of this class.
  fxl::WeakPtrFactory<FakeUserCommunicatorFactory> weak_ptr_factory_;
};

enum EnableP2PMesh { NO, YES };

}  // namespace

class LedgerAppInstanceFactoryImpl : public LedgerAppInstanceFactory {
 public:
  LedgerAppInstanceFactoryImpl(InjectNetworkError inject_network_error,
                               EnableP2PMesh enable_p2p_mesh)
      : services_loop_(&kAsyncLoopConfigNoAttachToThread),
        cloud_provider_(FakeCloudProvider::Builder().SetInjectNetworkError(
            inject_network_error)),
        enable_p2p_mesh_(enable_p2p_mesh) {}
  ~LedgerAppInstanceFactoryImpl() override;
  void Init();

  std::unique_ptr<LedgerAppInstance> NewLedgerAppInstance(
      LoopController* loop_controller) override;

 private:
  // Loop on which to run services.
  async::Loop services_loop_;
  fidl_helpers::BoundInterfaceSet<cloud_provider::CloudProvider,
                                  FakeCloudProvider>
      cloud_provider_;
  int app_instance_counter_ = 0;
  NetConnectorFactory netconnector_factory_;
  const EnableP2PMesh enable_p2p_mesh_;
};

void LedgerAppInstanceFactoryImpl::Init() { services_loop_.StartThread(); }

LedgerAppInstanceFactoryImpl::~LedgerAppInstanceFactoryImpl() {
  services_loop_.Quit();
  services_loop_.JoinThreads();
}

std::unique_ptr<LedgerAppInstanceFactory::LedgerAppInstance>
LedgerAppInstanceFactoryImpl::NewLedgerAppInstance(
    LoopController* loop_controller) {
  ledger_internal::LedgerRepositoryFactoryPtr repository_factory_ptr;
  fidl::InterfaceRequest<ledger_internal::LedgerRepositoryFactory>
      repository_factory_request = repository_factory_ptr.NewRequest();

  std::unique_ptr<p2p_sync::UserCommunicatorFactory> user_communicator_factory;
  if (enable_p2p_mesh_ == EnableP2PMesh::YES) {
    std::string host_name = "host_" + std::to_string(app_instance_counter_);
    user_communicator_factory = std::make_unique<FakeUserCommunicatorFactory>(
        services_loop_.dispatcher(), &netconnector_factory_,
        std::move(host_name));
  }
  auto result = std::make_unique<LedgerAppInstanceImpl>(
      loop_controller, services_loop_.dispatcher(),
      std::move(repository_factory_request), std::move(repository_factory_ptr),
      &cloud_provider_, std::move(user_communicator_factory));
  app_instance_counter_++;
  return result;
}

namespace {

class FactoryBuilderIntegrationImpl : public LedgerAppInstanceFactoryBuilder {
 public:
  FactoryBuilderIntegrationImpl(InjectNetworkError inject_error,
                                EnableP2PMesh enable_p2p)
      : inject_error_(inject_error), enable_p2p_(enable_p2p){};

  std::unique_ptr<LedgerAppInstanceFactory> NewFactory() const override {
    auto factory = std::make_unique<LedgerAppInstanceFactoryImpl>(inject_error_,
                                                                  enable_p2p_);
    factory->Init();
    return factory;
  }

 private:
  InjectNetworkError inject_error_;
  EnableP2PMesh enable_p2p_;
};

}  // namespace

std::vector<const LedgerAppInstanceFactoryBuilder*>
GetLedgerAppInstanceFactoryBuilders() {
  static std::vector<std::unique_ptr<FactoryBuilderIntegrationImpl>>
      static_builders;
  static std::once_flag flag;

  auto static_builders_ptr = &static_builders;
  std::call_once(flag, [&static_builders_ptr] {
    for (auto inject_error :
         {InjectNetworkError::NO, InjectNetworkError::YES}) {
      for (auto enable_p2p : {EnableP2PMesh::NO, EnableP2PMesh::YES}) {
        if (enable_p2p == EnableP2PMesh::YES &&
            inject_error != InjectNetworkError::YES) {
          // Only enable p2p when cloud has errors. This helps ensure our tests
          // are fast enough for the CQ.
          continue;
        }
        static_builders_ptr->push_back(
            std::make_unique<FactoryBuilderIntegrationImpl>(inject_error,
                                                            enable_p2p));
      }
    }
  });

  std::vector<const LedgerAppInstanceFactoryBuilder*> builders;

  for (const auto& builder : static_builders) {
    builders.push_back(builder.get());
  }

  return builders;
}

}  // namespace ledger
