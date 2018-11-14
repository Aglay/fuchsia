// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_P2P_PROVIDER_IMPL_USER_ID_PROVIDER_IMPL_H_
#define PERIDOT_BIN_LEDGER_P2P_PROVIDER_IMPL_USER_ID_PROVIDER_IMPL_H_

#include <string>

#include <fuchsia/modular/auth/cpp/fidl.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fit/function.h>

#include "peridot/bin/ledger/environment/environment.h"
#include "peridot/bin/ledger/filesystem/detached_path.h"
#include "peridot/bin/ledger/p2p_provider/public/user_id_provider.h"
#include "peridot/lib/firebase_auth/firebase_auth.h"

namespace p2p_provider {

// Implementation of |UserIdProvider| that retrieves the user id from the user
// Google profile using the given |TokenProvider| and caches this information in
// the filesystem.
class UserIdProviderImpl : public UserIdProvider {
 public:
  UserIdProviderImpl(
      ledger::Environment* environment, ledger::DetachedPath user_directory,
      fuchsia::modular::auth::TokenProviderPtr token_provider_ptr,
      std::string cobalt_client_name);
  ~UserIdProviderImpl() override;

  void GetUserId(fit::function<void(Status, std::string)> callback) override;

 private:
  bool LoadUserIdFromFile(std::string* id);
  bool UpdateUserId(std::string user_id);
  bool WriteUserIdToFile(std::string id);

  const ledger::DetachedPath user_id_path_;
  std::unique_ptr<firebase_auth::FirebaseAuth> firebase_auth_;
};

}  // namespace p2p_provider

#endif  // PERIDOT_BIN_LEDGER_P2P_PROVIDER_IMPL_USER_ID_PROVIDER_IMPL_H_
