// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fs/service.h>

#include <fuchsia/cpp/component.h>
#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/modular_auth.h>
#include "gtest/gtest.h"
#include "lib/app/cpp/service_provider_impl.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/agent_runner/agent_runner.h"
#include "peridot/bin/component/message_queue_manager.h"
#include "peridot/bin/entity/entity_provider_runner.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/ledger_client/page_id.h"
#include "peridot/lib/testing/fake_agent_runner_storage.h"
#include "peridot/lib/testing/fake_application_launcher.h"
#include "peridot/lib/testing/mock_base.h"
#include "peridot/lib/testing/test_with_ledger.h"

namespace modular {
namespace testing {
namespace {

class AgentRunnerTest : public TestWithLedger {
 public:
  AgentRunnerTest() = default;

  void SetUp() override {
    TestWithLedger::SetUp();

    mqm_.reset(new MessageQueueManager(
        ledger_client(), MakePageId("0123456789123456"), "/tmp/test_mq_data"));
    entity_provider_runner_.reset(new EntityProviderRunner(nullptr));
    agent_runner_.reset(
        new AgentRunner(&launcher_, mqm_.get(), ledger_repository(),
                        &agent_runner_storage_, token_provider_factory_.get(),
                        ui_provider_.get(), entity_provider_runner_.get()));
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

 private:
  FakeApplicationLauncher launcher_;

  std::unique_ptr<MessageQueueManager> mqm_;
  FakeAgentRunnerStorage agent_runner_storage_;
  std::unique_ptr<EntityProviderRunner> entity_provider_runner_;
  std::unique_ptr<AgentRunner> agent_runner_;

  modular_auth::TokenProviderFactoryPtr token_provider_factory_;
  modular::UserIntelligenceProviderPtr ui_provider_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentRunnerTest);
};

class MyDummyAgent : Agent,
                     public component::ApplicationController,
                     public testing::MockBase {
 public:
  MyDummyAgent(zx::channel directory_request,
               fidl::InterfaceRequest<component::ApplicationController> ctrl)
      : vfs_(async_get_default()),
        outgoing_directory_(fbl::AdoptRef(new fs::PseudoDir())),
        app_controller_(this, std::move(ctrl)),
        agent_binding_(this) {
    outgoing_directory_->AddEntry(
        Agent::Name_,
        fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
          agent_binding_.Bind(std::move(channel));
          return ZX_OK;
        })));
    vfs_.ServeDirectory(outgoing_directory_, std::move(directory_request));
  }

  void KillApplication() { app_controller_.Unbind(); }

  size_t GetCallCount(const std::string func) { return counts.count(func); }

 private:
  // |ApplicationController|
  void Kill() override { ++counts["Kill"]; }
  // |ApplicationController|
  void Detach() override { ++counts["Detach"]; }
  // |ApplicationController|
  void Wait(WaitCallback callback) override { ++counts["Wait"]; }

  // |Agent|
  void Connect(fidl::StringPtr /*requestor_url*/,
               fidl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    ++counts["Connect"];
  }

  // |Agent|
  void RunTask(fidl::StringPtr /*task_id*/,
               RunTaskCallback /*callback*/) override {
    ++counts["RunTask"];
  }

 private:
  fs::ManagedVfs vfs_;
  fbl::RefPtr<fs::PseudoDir> outgoing_directory_;
  fidl::Binding<component::ApplicationController> app_controller_;
  fidl::Binding<modular::Agent> agent_binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MyDummyAgent);
};

// Test that connecting to an agent will start it up.
// Then there should be an Agent.Connect().
TEST_F(AgentRunnerTest, ConnectToAgent) {
  int agent_launch_count = 0;
  std::unique_ptr<MyDummyAgent> dummy_agent;
  constexpr char kMyAgentUrl[] = "file:///my_agent";
  launcher()->RegisterApplication(
      kMyAgentUrl,
      [&dummy_agent, &agent_launch_count](
          component::ApplicationLaunchInfo launch_info,
          fidl::InterfaceRequest<component::ApplicationController> ctrl) {
        dummy_agent = std::make_unique<MyDummyAgent>(
            std::move(launch_info.directory_request), std::move(ctrl));
        ++agent_launch_count;
      });

  component::ServiceProviderPtr incoming_services;
  AgentControllerPtr agent_controller;
  agent_runner()->ConnectToAgent("requestor_url", kMyAgentUrl,
                                 incoming_services.NewRequest(),
                                 agent_controller.NewRequest());

  RunLoopUntilWithTimeout([&dummy_agent] {
    return dummy_agent && dummy_agent->GetCallCount("Connect") > 0;
  });
  EXPECT_EQ(1, agent_launch_count);
  dummy_agent->ExpectCalledOnce("Connect");
  dummy_agent->ExpectNoOtherCalls();

  // Connecting to the same agent again shouldn't launch a new instance and
  // shouldn't re-initialize the existing instance of the agent application,
  // but should call |Connect()|.

  AgentControllerPtr agent_controller2;
  component::ServiceProviderPtr incoming_services2;
  agent_runner()->ConnectToAgent("requestor_url2", kMyAgentUrl,
                                 incoming_services2.NewRequest(),
                                 agent_controller2.NewRequest());

  RunLoopUntilWithTimeout([&dummy_agent] {
    return dummy_agent && dummy_agent->GetCallCount("Connect");
  });
  EXPECT_EQ(1, agent_launch_count);
  dummy_agent->ExpectCalledOnce("Connect");
  dummy_agent->ExpectNoOtherCalls();
}

// Test that if an agent application dies, it is removed from agent runner
// (which means outstanding AgentControllers are closed).
TEST_F(AgentRunnerTest, AgentController) {
  std::unique_ptr<MyDummyAgent> dummy_agent;
  constexpr char kMyAgentUrl[] = "file:///my_agent";
  launcher()->RegisterApplication(
      kMyAgentUrl,
      [&dummy_agent](
          component::ApplicationLaunchInfo launch_info,
          fidl::InterfaceRequest<component::ApplicationController> ctrl) {
        dummy_agent = std::make_unique<MyDummyAgent>(
            std::move(launch_info.directory_request), std::move(ctrl));
      });

  component::ServiceProviderPtr incoming_services;
  AgentControllerPtr agent_controller;
  agent_runner()->ConnectToAgent("requestor_url", kMyAgentUrl,
                                 incoming_services.NewRequest(),
                                 agent_controller.NewRequest());

  RunLoopUntilWithTimeout([&dummy_agent] { return !!dummy_agent; });
  dummy_agent->KillApplication();

  // Agent application died, so check that AgentController dies here.
  agent_controller.set_error_handler(
      [&agent_controller] { agent_controller.Unbind(); });
  RunLoopUntilWithTimeout(
      [&agent_controller] { return !agent_controller.is_bound(); });
  EXPECT_FALSE(agent_controller.is_bound());
}

}  // namespace
}  // namespace testing
}  // namespace modular
