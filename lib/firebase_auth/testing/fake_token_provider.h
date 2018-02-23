// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_FIREBASE_AUTH_TESTING_FAKE_TOKEN_PROVIDER_H_
#define PERIDOT_LIB_FIREBASE_AUTH_TESTING_FAKE_TOKEN_PROVIDER_H_

#include <functional>

#include "lib/auth/fidl/token_provider.fidl.h"
#include "lib/fxl/macros.h"

namespace firebase_auth {
// FakeTokenProvider is a dummy implementation of a TokenProvider intended to be
// used to connect to unauthenticated firebase instances.
//
// The local ID Firebase token are set to a random UUID fixed at the
// construction time.
//
// Other token values are set to dummy const values.
class FakeTokenProvider : public modular::auth::TokenProvider {
 public:
  FakeTokenProvider();
  ~FakeTokenProvider() override {}

 private:
  void GetAccessToken(const GetAccessTokenCallback& callback) override;
  void GetIdToken(const GetIdTokenCallback& callback) override;
  void GetFirebaseAuthToken(
      const f1dl::String& firebase_api_key,
      const GetFirebaseAuthTokenCallback& callback) override;
  void GetClientId(const GetClientIdCallback& callback) override;

  std::string firebase_id_token_;
  std::string firebase_local_id_;
  std::string email_;
  std::string client_id_;
  FXL_DISALLOW_COPY_AND_ASSIGN(FakeTokenProvider);
};

}  // namespace firebase_auth

#endif  // PERIDOT_LIB_FIREBASE_AUTH_TESTING_FAKE_TOKEN_PROVIDER_H_
