// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_TESTS_MAXWELL_INTEGRATION_TEST_H_
#define PERIDOT_TESTS_MAXWELL_INTEGRATION_TEST_H_

#include "gtest/gtest.h"
#include "lib/app/cpp/connect.h"
#include "lib/app/cpp/service_provider_impl.h"
#include "lib/app/cpp/startup_context.h"
#include "lib/gtest/test_with_message_loop.h"
#include "peridot/bin/maxwell/agent_launcher.h"
#include "peridot/lib/testing/component_context_fake.h"
#include "peridot/lib/testing/entity_resolver_fake.h"

namespace maxwell {

class MaxwellTestBase : public gtest::TestWithMessageLoop {
 protected:
  MaxwellTestBase();
  virtual ~MaxwellTestBase() = default;

  void StartAgent(const std::string& url,
                  std::unique_ptr<MaxwellServiceProviderBridge> bridge) {
    agent_launcher_->StartAgent(url, std::move(bridge));
  }

  component::Services StartServices(const std::string& url);

  template <typename Interface>
  fidl::InterfacePtr<Interface> ConnectToService(const std::string& url) {
    auto services = StartServices(url);
    return services.ConnectToService<Interface>();
  }

  component::Environment* root_environment();

  fuchsia::modular::EntityResolverFake& entity_resolver() {
    return child_component_context_.entity_resolver_fake();
  }

 private:
  std::unique_ptr<component::StartupContext> startup_context_;
  std::unique_ptr<AgentLauncher> agent_launcher_;

  component::ServiceProviderImpl child_app_services_;
  fuchsia::modular::ComponentContextFake child_component_context_;
};

}  // namespace maxwell

#endif  // PERIDOT_TESTS_MAXWELL_INTEGRATION_TEST_H_
