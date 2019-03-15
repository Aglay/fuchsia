// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_LIB_FIREBASE_AUTH_TESTING_TEST_FIREBASE_AUTH_H_
#define SRC_LEDGER_LIB_FIREBASE_AUTH_TESTING_TEST_FIREBASE_AUTH_H_

#include <lib/async/dispatcher.h>
#include <lib/fit/function.h>

#include "src/ledger/lib/firebase_auth/firebase_auth.h"

namespace firebase_auth {

class TestFirebaseAuth : public FirebaseAuth {
 public:
  explicit TestFirebaseAuth(async_dispatcher_t* dispatcher);

  // FirebaseAuth:
  void set_error_handler(fit::closure on_error) override;

  fxl::RefPtr<callback::Cancellable> GetFirebaseToken(
      fit::function<void(AuthStatus, std::string)> callback) override;

  fxl::RefPtr<callback::Cancellable> GetFirebaseUserId(
      fit::function<void(AuthStatus, std::string)> callback) override;

  void TriggerConnectionErrorHandler();

  std::string token_to_return;

  AuthStatus status_to_return = AuthStatus::OK;

  std::string user_id_to_return;

 private:
  async_dispatcher_t* const dispatcher_;

  fit::closure error_handler_;
};

}  // namespace firebase_auth

#endif  // SRC_LEDGER_LIB_FIREBASE_AUTH_TESTING_TEST_FIREBASE_AUTH_H_
