// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_AGENT_CPP_AGENT_IMPL_H_
#define LIB_AGENT_CPP_AGENT_IMPL_H_

#include <memory>

#include <fuchsia/cpp/component.h>
#include <fuchsia/cpp/modular.h>

#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/interface_request.h"
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
    virtual void Connect(fidl::InterfaceRequest<component::ServiceProvider>
                             outgoing_services) = 0;
    virtual void RunTask(const fidl::StringPtr& task_id,
                         const std::function<void()>& done) = 0;
  };

  AgentImpl(component::ServiceNamespace* service_namespace, Delegate* delegate);

  AgentImpl(fbl::RefPtr<fs::PseudoDir> directory, Delegate* delegate);

 private:
  // |Agent|
  void Connect(fidl::StringPtr requestor_url,
               fidl::InterfaceRequest<component::ServiceProvider>
                   services_request) override;
  // |Agent|
  void RunTask(fidl::StringPtr task_id,
               RunTaskCallback callback) override;

  Delegate* const delegate_;
  fidl::Binding<Agent> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentImpl);
};

}  // namespace modular

#endif  // LIB_AGENT_CPP_AGENT_IMPL_H_
