// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firestore/app/factory_impl.h"

#include <fuchsia/ledger/cloud/firestore/cpp/fidl.h>
#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/gtest/test_loop_fixture.h>

#include "peridot/lib/firebase_auth/testing/test_token_provider.h"
#include "peridot/lib/rng/test_random.h"

namespace cloud_provider_firestore {

class FactoryImplTest : public gtest::TestLoopFixture {
 public:
  FactoryImplTest()
      : random_(test_loop().initial_state()),
        factory_impl_(dispatcher(), &random_, /*startup_context=*/nullptr,
                      /*cobalt_client_name=*/""),
        factory_binding_(&factory_impl_, factory_.NewRequest()),
        token_provider_(dispatcher()),
        token_provider_binding_(&token_provider_) {}
  ~FactoryImplTest() override {}

 protected:
  rng::TestRandom random_;
  FactoryImpl factory_impl_;
  FactoryPtr factory_;
  fidl::Binding<Factory> factory_binding_;

  firebase_auth::TestTokenProvider token_provider_;
  fidl::Binding<fuchsia::modular::auth::TokenProvider> token_provider_binding_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(FactoryImplTest);
};

TEST_F(FactoryImplTest, GetCloudProvider) {
  bool callback_called = false;
  token_provider_.Set("this is a token", "some id", "me@example.com");

  cloud_provider::Status status = cloud_provider::Status::INTERNAL_ERROR;
  cloud_provider::CloudProviderPtr cloud_provider;
  Config config;
  config.server_id = "some server id";
  config.api_key = "some api key";
  factory_->GetCloudProvider(
      std::move(config), token_provider_binding_.NewBinding(),
      cloud_provider.NewRequest(),
      callback::Capture(callback::SetWhenCalled(&callback_called), &status));
  RunLoopUntilIdle();
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(cloud_provider::Status::OK, status);

  callback_called = false;
  factory_impl_.ShutDown(callback::SetWhenCalled(&callback_called));
  RunLoopUntilIdle();
  EXPECT_TRUE(callback_called);
}

}  // namespace cloud_provider_firestore
