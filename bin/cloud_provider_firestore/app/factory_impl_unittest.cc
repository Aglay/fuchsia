// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firestore/app/factory_impl.h"

#include <fuchsia/cpp/cloud_provider_firestore.h>
#include "garnet/lib/callback/capture.h"
#include "garnet/lib/gtest/test_with_message_loop.h"
#include "lib/fidl/cpp/binding.h"
#include "peridot/lib/firebase_auth/testing/test_token_provider.h"

namespace cloud_provider_firestore {

class FactoryImplTest : public gtest::TestWithMessageLoop {
 public:
  FactoryImplTest()
      : factory_impl_(message_loop_.async()),
        factory_binding_(&factory_impl_, factory_.NewRequest()),
        token_provider_(message_loop_.task_runner()),
        token_provider_binding_(&token_provider_) {}
  ~FactoryImplTest() override {}

 protected:
  FactoryImpl factory_impl_;
  FactoryPtr factory_;
  fidl::Binding<Factory> factory_binding_;

  firebase_auth::TestTokenProvider token_provider_;
  fidl::Binding<modular_auth::TokenProvider> token_provider_binding_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(FactoryImplTest);
};

TEST_F(FactoryImplTest, GetCloudProvider) {
  token_provider_.Set("this is a token", "some id", "me@example.com");

  cloud_provider::Status status = cloud_provider::Status::INTERNAL_ERROR;
  cloud_provider::CloudProviderPtr cloud_provider;
  Config config;
  config.server_id = "some server id";
  config.api_key = "some api key";
  factory_->GetCloudProvider(
      std::move(config), token_provider_binding_.NewBinding(),
      cloud_provider.NewRequest(), callback::Capture([] {}, &status));
  RunLoopUntilIdle();
  EXPECT_EQ(cloud_provider::Status::OK, status);

  bool called = false;
  factory_impl_.ShutDown([&called] { called = true; });
  RunLoopUntilIdle();
  EXPECT_TRUE(called);
}

}  // namespace cloud_provider_firestore
