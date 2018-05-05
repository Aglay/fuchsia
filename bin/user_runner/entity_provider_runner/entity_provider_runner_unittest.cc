// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fs/service.h>

#include <fuchsia/cpp/component.h>
#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/modular_auth.h>
#include "gtest/gtest.h"
#include "lib/agent/cpp/agent_impl.h"
#include "lib/app/cpp/connect.h"
#include "lib/app/cpp/service_provider_impl.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/user_runner/agent_runner/agent_runner.h"
#include "peridot/bin/user_runner/message_queue/message_queue_manager.h"
#include "peridot/bin/user_runner/entity_provider_runner/entity_provider_launcher.h"
#include "peridot/bin/user_runner/entity_provider_runner/entity_provider_runner.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/ledger_client/page_id.h"
#include "peridot/lib/testing/fake_agent_runner_storage.h"
#include "peridot/lib/testing/fake_application_launcher.h"
#include "peridot/lib/testing/mock_base.h"
#include "peridot/lib/testing/test_with_ledger.h"

namespace modular {
namespace testing {
namespace {

class EntityProviderRunnerTest : public TestWithLedger, EntityProviderLauncher {
 public:
  EntityProviderRunnerTest() = default;

  void SetUp() override {
    TestWithLedger::SetUp();

    mqm_.reset(new MessageQueueManager(
        ledger_client(), MakePageId("0123456789123456"), mq_data_dir_.path()));
    entity_provider_runner_.reset(
        new EntityProviderRunner(static_cast<EntityProviderLauncher*>(this)));
    // The |UserIntelligenceProvider| below must be nullptr in order for agent
    // creation to be synchronous, which these tests assume.
    agent_runner_.reset(new AgentRunner(
        &launcher_, mqm_.get(), ledger_repository(), &agent_runner_storage_,
        token_provider_factory_.get(), nullptr, entity_provider_runner_.get()));
  }

  void TearDown() override {
    agent_runner_.reset();
    entity_provider_runner_.reset();
    mqm_.reset();

    TestWithLedger::TearDown();
  }

  MessageQueueManager* message_queue_manager() { return mqm_.get(); }

 protected:
  AgentRunner* agent_runner() { return agent_runner_.get(); }
  FakeApplicationLauncher* launcher() { return &launcher_; }
  EntityProviderRunner* entity_provider_runner() {
    return entity_provider_runner_.get();
  }

 private:
  // TODO(vardhan): A test probably shouldn't be implementing this..
  // |EntityProviderLauncher|
  void ConnectToEntityProvider(
      const std::string& agent_url,
      fidl::InterfaceRequest<EntityProvider> entity_provider_request,
      fidl::InterfaceRequest<AgentController> agent_controller_request)
      override {
    agent_runner_->ConnectToEntityProvider(agent_url,
                                           std::move(entity_provider_request),
                                           std::move(agent_controller_request));
  }

  FakeApplicationLauncher launcher_;

  files::ScopedTempDir mq_data_dir_;
  std::unique_ptr<MessageQueueManager> mqm_;
  FakeAgentRunnerStorage agent_runner_storage_;
  std::unique_ptr<EntityProviderRunner> entity_provider_runner_;
  std::unique_ptr<AgentRunner> agent_runner_;

  modular_auth::TokenProviderFactoryPtr token_provider_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(EntityProviderRunnerTest);
};

class MyEntityProvider : AgentImpl::Delegate,
                         EntityProvider,
                         public component::ApplicationController,
                         public testing::MockBase {
 public:
  MyEntityProvider(
      component::ApplicationLaunchInfo launch_info,
      fidl::InterfaceRequest<component::ApplicationController> ctrl)
      : vfs_(async_get_default()),
        outgoing_directory_(fbl::AdoptRef(new fs::PseudoDir())),
        app_controller_(this, std::move(ctrl)),
        entity_provider_binding_(this),
        launch_info_(std::move(launch_info)) {
    outgoing_directory_->AddEntry(
        EntityProvider::Name_,
        fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
          entity_provider_binding_.Bind(std::move(channel));
          return ZX_OK;
        })));
    vfs_.ServeDirectory(outgoing_directory_,
                        std::move(launch_info_.directory_request));
    agent_impl_ = std::make_unique<AgentImpl>(
        outgoing_directory_, static_cast<AgentImpl::Delegate*>(this));

    // Get |agent_context_| and |entity_resolver_| from incoming namespace.
    FXL_CHECK(launch_info_.additional_services);
    FXL_CHECK(launch_info_.additional_services->provider.is_valid());
    auto additional_services =
        launch_info_.additional_services->provider.Bind();
    component::ConnectToService(additional_services.get(),
                                agent_context_.NewRequest());
    ComponentContextPtr component_context;
    agent_context_->GetComponentContext(component_context.NewRequest());
    component_context->GetEntityResolver(entity_resolver_.NewRequest());
  }

  size_t GetCallCount(const std::string func) { return counts.count(func); }
  EntityResolver* entity_resolver() { return entity_resolver_.get(); }
  AgentContext* agent_context() { return agent_context_.get(); }

 private:
  // |ApplicationController|
  void Kill() override { ++counts["Kill"]; }
  // |ApplicationController|
  void Detach() override { ++counts["Detach"]; }
  // |ApplicationController|
  void Wait(WaitCallback callback) override { ++counts["Wait"]; }

  // |AgentImpl::Delegate|
  void Connect(fidl::InterfaceRequest<component::ServiceProvider>
                   outgoing_services) override {
    ++counts["Connect"];
  }
  // |AgentImpl::Delegate|
  void RunTask(const fidl::StringPtr& task_id,
               const std::function<void()>& done) override {
    ++counts["RunTask"];
    done();
  }

  // |EntityProvider|
  void GetTypes(fidl::StringPtr cookie, GetTypesCallback callback) override {
    fidl::VectorPtr<fidl::StringPtr> types;
    types.push_back("MyType");
    callback(std::move(types));
  }

  // |EntityProvider|
  void GetData(fidl::StringPtr cookie,
               fidl::StringPtr type,
               GetDataCallback callback) override {
    callback(type.get() + ":MyData");
  }

 private:
  fs::ManagedVfs vfs_;
  fbl::RefPtr<fs::PseudoDir> outgoing_directory_;
  AgentContextPtr agent_context_;
  std::unique_ptr<AgentImpl> agent_impl_;
  EntityResolverPtr entity_resolver_;
  fidl::Binding<component::ApplicationController> app_controller_;
  fidl::Binding<modular::EntityProvider> entity_provider_binding_;
  component::ApplicationLaunchInfo launch_info_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MyEntityProvider);
};

TEST_F(EntityProviderRunnerTest, Basic) {
  std::unique_ptr<MyEntityProvider> dummy_agent;
  constexpr char kMyAgentUrl[] = "file:///my_agent";
  launcher()->RegisterApplication(
      kMyAgentUrl,
      [&dummy_agent](
          component::ApplicationLaunchInfo launch_info,
          fidl::InterfaceRequest<component::ApplicationController> ctrl) {
        dummy_agent = std::make_unique<MyEntityProvider>(std::move(launch_info),
                                                         std::move(ctrl));
      });

  // 1. Start up the entity provider agent.
  component::ServiceProviderPtr incoming_services;
  AgentControllerPtr agent_controller;
  agent_runner()->ConnectToAgent("dummy_requestor_url", kMyAgentUrl,
                                 incoming_services.NewRequest(),
                                 agent_controller.NewRequest());

  RunLoopUntilWithTimeout([&dummy_agent] {
    return dummy_agent.get() != nullptr &&
           dummy_agent->GetCallCount("Connect") == 1;
  });
  dummy_agent->ExpectCalledOnce("Connect");

  // 2. Make an entity reference on behalf of this agent.
  // The framework should use |kMyAgentUrl| as the agent to associate new
  // references.
  EntityReferenceFactoryPtr factory;
  dummy_agent->agent_context()->GetEntityReferenceFactory(factory.NewRequest());
  fidl::StringPtr entity_ref;
  factory->CreateReference("my_cookie", [&entity_ref](fidl::StringPtr retval) {
    entity_ref = retval;
  });

  RunLoopUntilWithTimeout([&entity_ref] { return !entity_ref.is_null(); });
  EXPECT_FALSE(entity_ref.is_null());

  // 3. Resolve the reference into an |Entity|, make calls to GetTypes and
  //    GetData, which should route into our |MyEntityProvider|.
  EntityPtr entity;
  dummy_agent->entity_resolver()->ResolveEntity(entity_ref,
                                                entity.NewRequest());

  std::map<std::string, uint32_t> counts;
  entity->GetTypes([&counts](const fidl::VectorPtr<fidl::StringPtr>& types) {
    EXPECT_EQ(1u, types->size());
    EXPECT_EQ("MyType", types->at(0));
    counts["GetTypes"]++;
  });
  entity->GetData("MyType", [&counts](fidl::StringPtr data) {
    EXPECT_EQ("MyType:MyData", data.get());
    counts["GetData"]++;
  });
  RunLoopUntilWithTimeout(
      [&counts] { return counts["GetTypes"] == 1 && counts["GetData"] == 1; });
  EXPECT_EQ(1u, counts["GetTypes"]);
  EXPECT_EQ(1u, counts["GetData"]);
}

TEST_F(EntityProviderRunnerTest, DataEntity) {
  std::map<std::string, std::string> data;
  data["type1"] = "data1";

  auto entity_ref = entity_provider_runner()->CreateReferenceFromData(data);

  EntityResolverPtr entity_resolver;
  entity_provider_runner()->ConnectEntityResolver(entity_resolver.NewRequest());
  EntityPtr entity;
  entity_resolver->ResolveEntity(entity_ref, entity.NewRequest());

  fidl::VectorPtr<fidl::StringPtr> output_types;
  entity->GetTypes([&output_types](fidl::VectorPtr<fidl::StringPtr> result) {
    output_types = std::move(result);
  });
  RunLoopUntilWithTimeout([&output_types] { return !output_types.is_null(); });

  EXPECT_EQ(data.size(), output_types->size());
  EXPECT_EQ("type1", output_types->at(0));

  fidl::StringPtr output_data;
  entity->GetData("type1", [&output_data](fidl::StringPtr result) {
    output_data = result;
  });
  RunLoopUntilWithTimeout([&output_data] { return !output_data.is_null(); });
  EXPECT_EQ("data1", output_data);
}

}  // namespace
}  // namespace testing
}  // namespace modular
