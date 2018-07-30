// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/app_driver/cpp/agent_driver.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fxl/logging.h>
#include <lib/message_queue/cpp/message_queue_client.h>
#include <test/peridot/tests/queuepersistence/cpp/fidl.h>

#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/queue_persistence/defs.h"

using modular::testing::TestPoint;
using namespace test::peridot::tests::queuepersistence;

namespace {

// Cf. README.md for what this test does and how.
class TestApp : QueuePersistenceTestService {
 public:
  TestApp(modular::AgentHost* agent_host) {
    modular::testing::Init(agent_host->startup_context(), __FILE__);
    agent_host->agent_context()->GetComponentContext(
        component_context_.NewRequest());

    // Create a message queue and schedule a task to be run on receiving a
    // message on it.
    component_context_->ObtainMessageQueue("Test Queue",
                                           msg_queue_.NewRequest());
    msg_queue_.RegisterReceiver(
        [this](std::string message, fit::function<void()> ack) {
          ack();
          modular::testing::GetStore()->Put(
              "queue_persistence_test_agent_received_message", "", [] {});
        });

    services_.AddService<QueuePersistenceTestService>(
        [this](fidl::InterfaceRequest<QueuePersistenceTestService> request) {
          services_bindings_.AddBinding(this, std::move(request));
        });

    initialized_.Pass();
  }

  // Called by AgentDriver.
  void Connect(fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> services) {
    services_.AddBinding(std::move(services));
    modular::testing::GetStore()->Put("queue_persistence_test_agent_connected",
                                      "", [] {});
  }

  // Called by AgentDriver.
  void RunTask(const fidl::StringPtr& /*task_id*/,
               const std::function<void()>& /*callback*/) {}

  // Called by AgentDriver.
  void Terminate(const std::function<void()>& done) {
    // Stop processing messages, since we do async operations below and don't
    // want our receiver to fire.
    msg_queue_.RegisterReceiver(nullptr);

    modular::testing::GetStore()->Put("queue_persistence_test_agent_stopped",
                                      "",
                                      [done] { modular::testing::Done(done); });
  }

 private:
  // |QueuePersistenceTestService|
  void GetMessageQueueToken(GetMessageQueueTokenCallback callback) override {
    msg_queue_.GetToken(
        [callback](const fidl::StringPtr& token) { callback(token); });
  }

  TestPoint initialized_{"Queue persistence test agent initialized"};

  fuchsia::modular::ComponentContextPtr component_context_;
  modular::MessageQueueClient msg_queue_;

  component::ServiceNamespace services_;
  fidl::BindingSet<QueuePersistenceTestService> services_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = component::StartupContext::CreateFromStartupInfo();
  modular::AgentDriver<TestApp> driver(context.get(), [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
