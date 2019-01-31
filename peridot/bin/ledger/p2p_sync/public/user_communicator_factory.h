// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_P2P_SYNC_PUBLIC_USER_COMMUNICATOR_FACTORY_H_
#define PERIDOT_BIN_LEDGER_P2P_SYNC_PUBLIC_USER_COMMUNICATOR_FACTORY_H_

#include <memory>

#include "peridot/bin/ledger/p2p_provider/public/user_id_provider.h"
#include "peridot/bin/ledger/p2p_sync/public/user_communicator.h"

namespace p2p_sync {
// Factory for creating UserCommunicators with default configuration.
class UserCommunicatorFactory {
 public:
  UserCommunicatorFactory() {}
  virtual ~UserCommunicatorFactory() {}

  virtual std::unique_ptr<UserCommunicator> GetUserCommunicator(
      std::unique_ptr<p2p_provider::UserIdProvider> user_id_provider) = 0;
};

}  // namespace p2p_sync

#endif  // PERIDOT_BIN_LEDGER_P2P_SYNC_PUBLIC_USER_COMMUNICATOR_FACTORY_H_
