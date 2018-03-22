// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/tests/maxwell_integration/context_engine_test_base.h"

#include "peridot/lib/util/wait_until_idle.h"

namespace maxwell {

void ContextEngineTestBase::SetUp() {
  context_engine_ = ConnectToService<ContextEngine>("context_engine");
  context_engine_->GetContextDebug(debug_.NewRequest());
}

void ContextEngineTestBase::StartContextAgent(const std::string& url) {
  auto agent_bridge =
      std::make_unique<MaxwellServiceProviderBridge>(root_environment());
  agent_bridge->AddService<ContextWriter>(
      [this, url](f1dl::InterfaceRequest<ContextWriter> request) {
        auto scope = ComponentScope::New();
        auto agent_scope = AgentScope::New();
        agent_scope->url = url;
        scope->set_agent_scope(std::move(agent_scope));
        context_engine_->GetWriter(std::move(scope), std::move(request));
      });
  agent_bridge->AddService<ContextReader>(
      [this, url](f1dl::InterfaceRequest<ContextReader> request) {
        auto scope = ComponentScope::New();
        auto agent_scope = AgentScope::New();
        agent_scope->url = url;
        scope->set_agent_scope(std::move(agent_scope));
        context_engine_->GetReader(std::move(scope), std::move(request));
      });
  StartAgent(url, std::move(agent_bridge));
}

void ContextEngineTestBase::WaitUntilIdle() {
  util::WaitUntilIdle(debug_.get());
}

}  // namespace maxwell
