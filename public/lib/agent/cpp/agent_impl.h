// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_AGENT_CPP_AGENT_IMPL_H_
#define LIB_AGENT_CPP_AGENT_IMPL_H_

#include <memory>

#include "lib/agent/fidl/agent.fidl.h"
#include "lib/agent/fidl/agent_context.fidl.h"
#include "lib/app/fidl/service_provider.fidl.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/fxl/macros.h"
#include "lib/svc/cpp/service_namespace.h"

namespace modular {

// Use this class to talk to the modular framework as an Agent.
class AgentImpl : public Agent {
 public:
  // Users of AgentImpl register a delegate to receive messages from the
  // framework.
  class Delegate {
   public:
    virtual void Connect(
        f1dl::InterfaceRequest<app::ServiceProvider> outgoing_services) = 0;
    virtual void RunTask(const f1dl::String& task_id,
                         const std::function<void()>& done) = 0;
  };

  AgentImpl(app::ServiceNamespace* service_namespace, Delegate* delegate);

 private:
  // |Agent|
  void Connect(
      const f1dl::String& requestor_url,
      f1dl::InterfaceRequest<app::ServiceProvider> services_request) override;
  // |Agent|
  void RunTask(const f1dl::String& task_id,
               const RunTaskCallback& callback) override;

  Delegate* const delegate_;
  f1dl::Binding<Agent> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentImpl);
};

}  // namespace modular

#endif  // LIB_AGENT_CPP_AGENT_IMPL_H_
