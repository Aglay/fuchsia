// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/context/fidl/context_engine.fidl.h"
#include "lib/user_intelligence/fidl/scope.fidl.h"
#include "peridot/tests/maxwell_integration/test.h"

namespace maxwell {

// Base fixture to support test cases requiring Context Engine.
class ContextEngineTestBase : public MaxwellTestBase {
 public:
  void SetUp() override {
    context_engine_ = ConnectToService<ContextEngine>("context_engine");
  }

 protected:
  void StartContextAgent(const std::string& url) {
    auto agent_host =
        std::make_unique<ApplicationEnvironmentHostImpl>(root_environment());
    agent_host->AddService<ContextWriter>(
        [this, url](f1dl::InterfaceRequest<ContextWriter> request) {
          auto scope = ComponentScope::New();
          auto agent_scope = AgentScope::New();
          agent_scope->url = url;
          scope->set_agent_scope(std::move(agent_scope));
          context_engine_->GetWriter(std::move(scope), std::move(request));
        });
    agent_host->AddService<ContextReader>(
        [this, url](f1dl::InterfaceRequest<ContextReader> request) {
          auto scope = ComponentScope::New();
          auto agent_scope = AgentScope::New();
          agent_scope->url = url;
          scope->set_agent_scope(std::move(agent_scope));
          context_engine_->GetReader(std::move(scope), std::move(request));
        });
    StartAgent(url, std::move(agent_host));
  }

  ContextEngine* context_engine() { return context_engine_.get(); }

 private:
  ContextEnginePtr context_engine_;
};

}  // namespace maxwell
