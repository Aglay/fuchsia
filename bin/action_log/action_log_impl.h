// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_ACTION_LOG_ACTION_LOG_IMPL_H_
#define PERIDOT_BIN_ACTION_LOG_ACTION_LOG_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include <fuchsia/cpp/modular.h>
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fidl/cpp/interface_ptr_set.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/action_log/action_log_data.h"

namespace modular {

class UserActionLogImpl : public UserActionLog {
 public:
  UserActionLogImpl(ProposalPublisherPtr proposal_publisher);
  ~UserActionLogImpl() override;

 private:
  void GetComponentActionLog(
      ComponentScope scope,
      fidl::InterfaceRequest<ComponentActionLog> action_log_request) override;

  void Subscribe(
      fidl::InterfaceHandle<ActionLogListener> listener_handle) override;

  void Duplicate(fidl::InterfaceRequest<UserActionLog> request) override;

  void BroadcastToSubscribers(const ActionData& action_data);

  void MaybeProposeSharingVideo(const ActionData& action_data);

  void MaybeRecordEmailRecipient(const ActionData& action_data);

  ActionLogData action_log_;
  ProposalPublisherPtr proposal_publisher_;
  fidl::BindingSet<ComponentActionLog, std::unique_ptr<ComponentActionLog>>
      action_log_bindings_;
  fidl::InterfacePtrSet<ActionLogListener> subscribers_;
  fidl::BindingSet<UserActionLog> bindings_;
  std::string last_email_rcpt_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UserActionLogImpl);
};

class ComponentActionLogImpl : public ComponentActionLog {
 public:
  ComponentActionLogImpl(ActionLogger log_action);
  ~ComponentActionLogImpl() override;

  void LogAction(fidl::StringPtr method, fidl::StringPtr params) override;

 private:
  const ActionLogger log_action_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ComponentActionLogImpl);
};
}  // namespace modular

#endif  // PERIDOT_BIN_ACTION_LOG_ACTION_LOG_IMPL_H_
