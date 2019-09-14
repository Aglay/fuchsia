// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_AGENT_RUNNER_AGENT_RUNNER_STORAGE_IMPL_H_
#define SRC_MODULAR_BIN_SESSIONMGR_AGENT_RUNNER_AGENT_RUNNER_STORAGE_IMPL_H_

#include <fuchsia/ledger/cpp/fidl.h>

#include <src/lib/fxl/macros.h>

#include "peridot/lib/ledger_client/ledger_client.h"
#include "peridot/lib/ledger_client/page_client.h"
#include "peridot/lib/ledger_client/types.h"
#include "src/modular/bin/sessionmgr/agent_runner/agent_runner_storage.h"
#include "src/modular/lib/async/cpp/operation.h"

namespace modular {

// An implementation of |AgentRunnerStorage| that persists data in the ledger.
class AgentRunnerStorageImpl : public AgentRunnerStorage, PageClient {
 public:
  explicit AgentRunnerStorageImpl(LedgerClient* ledger_client, fuchsia::ledger::PageId page_id);
  ~AgentRunnerStorageImpl() override;

 private:
  // |AgentRunnerStorage|
  void Initialize(NotificationDelegate* delegate, fit::function<void()> done) override;

  // Operation subclasses:
  class InitializeCall;

  // |PageClient|

  void OnPageChange(const std::string& key, const std::string& value) override;
  // |PageClient|
  void OnPageDelete(const std::string& key) override;

  // Only valid after |Initialize()| is called.
  NotificationDelegate* delegate_;  // Not owned.

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentRunnerStorageImpl);
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONMGR_AGENT_RUNNER_AGENT_RUNNER_STORAGE_IMPL_H_
