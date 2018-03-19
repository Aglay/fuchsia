// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_COMPONENT_COMPONENT_CONTEXT_IMPL_H_
#define PERIDOT_BIN_COMPONENT_COMPONENT_CONTEXT_IMPL_H_

#include <string>

#include "lib/component/fidl/component_context.fidl.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/fidl/cpp/bindings/string.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/component/message_queue_manager.h"
#include "peridot/bin/entity/entity_provider_runner.h"
#include "peridot/bin/ledger/fidl/internal.fidl.h"

namespace modular {

class AgentRunner;

// The parameters of component context that do not vary by instance.
struct ComponentContextInfo {
  MessageQueueManager* const message_queue_manager;
  AgentRunner* const agent_runner;
  ledger::LedgerRepository* const ledger_repository;
  EntityProviderRunner* const entity_provider_runner;
};

// Implements the ComponentContext interface, which is provided to
// modules and agents. The interface is public, because the class
// doesn't contain the Bindings for this interface. TODO(mesch): Move
// bindings into the class.
class ComponentContextImpl : public ComponentContext {
 public:
  // * A component namespace identifies components whose lifetimes are related,
  //   where all of their persisted information will live together; for modules
  //   this is the story id, for agents it is kAgentComponentNamespace, etc.
  // * A component instance ID identifies a particular instance of a component;
  //   for modules, this is the module path in their story. For agents, it is
  //   the agent URL.
  // * A component URL is the origin from which the executable associated with
  //   the component was fetched from.
  explicit ComponentContextImpl(const ComponentContextInfo& info,
                                std::string component_namespace,
                                std::string component_instance_id,
                                std::string component_url);

  ~ComponentContextImpl() override;

  const std::string& component_instance_id() { return component_instance_id_; }

  void Connect(f1dl::InterfaceRequest<ComponentContext> request);
  ComponentContextPtr NewBinding();

 private:
  // |ComponentContext|
  void GetLedger(f1dl::InterfaceRequest<ledger::Ledger> request,
                 const GetLedgerCallback& result) override;

  // |ComponentContext|
  void ConnectToAgent(const f1dl::StringPtr& url,
                      f1dl::InterfaceRequest<component::ServiceProvider>
                          incoming_services_request,
                      f1dl::InterfaceRequest<AgentController>
                          agent_controller_request) override;

  // |ComponentContext|
  void ObtainMessageQueue(
      const f1dl::StringPtr& name,
      f1dl::InterfaceRequest<MessageQueue> request) override;

  // |ComponentContext|
  void DeleteMessageQueue(const f1dl::StringPtr& name) override;

  // |ComponentContext|
  void GetMessageSender(const f1dl::StringPtr& queue_token,
                        f1dl::InterfaceRequest<MessageSender> request) override;

  // |ComponentContext|
  void GetEntityResolver(
      f1dl::InterfaceRequest<EntityResolver> request) override;

  // |ComponentContext|
  void CreateEntityWithData(
      f1dl::VectorPtr<TypeToDataEntryPtr> type_to_data,
      const CreateEntityWithDataCallback& result) override;

  MessageQueueManager* const message_queue_manager_;
  AgentRunner* const agent_runner_;
  ledger::LedgerRepository* const ledger_repository_;
  EntityProviderRunner* const entity_provider_runner_;

  const std::string component_namespace_;
  const std::string component_instance_id_;
  const std::string component_url_;

  f1dl::BindingSet<ComponentContext> bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ComponentContextImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_COMPONENT_COMPONENT_CONTEXT_IMPL_H_
