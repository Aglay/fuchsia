// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTS_E2E_SYNC_LEDGER_APP_INSTANCE_FACTORY_E2E_H_
#define PERIDOT_BIN_LEDGER_TESTS_E2E_SYNC_LEDGER_APP_INSTANCE_FACTORY_E2E_H_

#include <memory>
#include <string>

#include "peridot/bin/cloud_provider_firestore/testing/cloud_provider_factory.h"
#include "peridot/bin/ledger/testing/ledger_app_instance_factory.h"
#include "peridot/bin/ledger/testing/sync_params.h"
#include "peridot/lib/rng/system_random.h"

namespace ledger {

class LedgerAppInstanceFactoryImpl : public LedgerAppInstanceFactory {
 public:
  explicit LedgerAppInstanceFactoryImpl(
      std::unique_ptr<LoopController> loop_controller, SyncParams sync_params);
  ~LedgerAppInstanceFactoryImpl() override;

  std::unique_ptr<LedgerAppInstance> NewLedgerAppInstance() override;
  LoopController* GetLoopController() override;
  rng::Random* GetRandom() override;

 private:
  std::unique_ptr<LoopController> loop_controller_;
  rng::SystemRandom random_;
  const SyncParams sync_params_;
  const cloud_provider_firestore::CloudProviderFactory::UserId user_id_;
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTS_E2E_SYNC_LEDGER_APP_INSTANCE_FACTORY_E2E_H_
