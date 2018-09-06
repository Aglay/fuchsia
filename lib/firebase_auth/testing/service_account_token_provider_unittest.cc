// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/firebase_auth/testing/service_account_token_provider.h"

#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/strings/string_number_conversions.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/network_wrapper/fake_network_wrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "peridot/lib/firebase_auth/testing/service_account_test_constants.h"

namespace service_account {
namespace {
namespace http = ::fuchsia::net::oldhttp;

class ServiceAccountTokenProviderTest : public gtest::TestLoopFixture {
 public:
  ServiceAccountTokenProviderTest()
      : network_wrapper_(dispatcher()),
        token_provider_(&network_wrapper_,
                        Credentials::Parse(kTestServiceAccountConfig),
                        "user_id") {}

 protected:
  std::string GetSuccessResponseBody(std::string token, size_t expiration) {
    rapidjson::StringBuffer string_buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(string_buffer);

    writer.StartObject();

    writer.Key("idToken");
    writer.String(token);

    writer.Key("expiresIn");
    writer.String(fxl::NumberToString(expiration));

    writer.EndObject();

    return std::string(string_buffer.GetString(), string_buffer.GetSize());
  }

  http::URLResponse GetResponse(http::HttpErrorPtr error, uint32_t status,
                                std::string body) {
    http::URLResponse response;
    response.error = std::move(error);
    response.status_code = status;
    fsl::SizedVmo buffer;
    if (!fsl::VmoFromString(body, &buffer)) {
      ADD_FAILURE() << "Unable to convert string to Vmo.";
    }
    response.body = http::URLBody::New();
    response.body->set_sized_buffer(std::move(buffer).ToTransport());
    return response;
  }

  bool GetToken(std::string api_key,
                fuchsia::modular::auth::FirebaseTokenPtr* token,
                fuchsia::modular::auth::AuthErr* error) {
    bool called;
    token_provider_.GetFirebaseAuthToken(
        api_key,
        callback::Capture(callback::SetWhenCalled(&called), token, error));
    RunLoopUntilIdle();
    return called;
  }

  network_wrapper::FakeNetworkWrapper network_wrapper_;
  ServiceAccountTokenProvider token_provider_;
};

TEST_F(ServiceAccountTokenProviderTest, GetToken) {
  network_wrapper_.SetResponse(
      GetResponse(nullptr, 200, GetSuccessResponseBody("token", 3600)));

  fuchsia::modular::auth::FirebaseTokenPtr token;
  fuchsia::modular::auth::AuthErr error;
  ASSERT_TRUE(GetToken("api_key", &token, &error));
  ASSERT_EQ(fuchsia::modular::auth::Status::OK, error.status) << error.message;
  ASSERT_EQ("token", token->id_token);
}

TEST_F(ServiceAccountTokenProviderTest, GetTokenFromCache) {
  network_wrapper_.SetResponse(
      GetResponse(nullptr, 200, GetSuccessResponseBody("token", 3600)));

  fuchsia::modular::auth::FirebaseTokenPtr token;
  fuchsia::modular::auth::AuthErr error;

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::OK, error.status) << error.message;
  EXPECT_EQ("token", token->id_token);
  EXPECT_TRUE(network_wrapper_.GetRequest());

  network_wrapper_.ResetRequest();
  network_wrapper_.SetResponse(
      GetResponse(nullptr, 200, GetSuccessResponseBody("token2", 3600)));

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::OK, error.status) << error.message;
  EXPECT_EQ("token", token->id_token);
  EXPECT_FALSE(network_wrapper_.GetRequest());
}

TEST_F(ServiceAccountTokenProviderTest, GetTokenNoCacheCache) {
  network_wrapper_.SetResponse(
      GetResponse(nullptr, 200, GetSuccessResponseBody("token", 0)));

  fuchsia::modular::auth::FirebaseTokenPtr token;
  fuchsia::modular::auth::AuthErr error;

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::OK, error.status) << error.message;
  EXPECT_EQ("token", token->id_token);
  EXPECT_TRUE(network_wrapper_.GetRequest());

  network_wrapper_.SetResponse(
      GetResponse(nullptr, 200, GetSuccessResponseBody("token2", 0)));

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::OK, error.status) << error.message;
  EXPECT_EQ("token2", token->id_token);
  EXPECT_TRUE(network_wrapper_.GetRequest());
}

TEST_F(ServiceAccountTokenProviderTest, NetworkError) {
  auto network_error = http::HttpError::New();
  network_error->description = "Error";

  network_wrapper_.SetResponse(GetResponse(std::move(network_error), 0, ""));

  fuchsia::modular::auth::FirebaseTokenPtr token;
  fuchsia::modular::auth::AuthErr error;

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::NETWORK_ERROR, error.status);
  EXPECT_FALSE(token);
  EXPECT_TRUE(network_wrapper_.GetRequest());
}

TEST_F(ServiceAccountTokenProviderTest, AuthenticationError) {
  network_wrapper_.SetResponse(GetResponse(nullptr, 401, "Unauthorized"));

  fuchsia::modular::auth::FirebaseTokenPtr token;
  fuchsia::modular::auth::AuthErr error;

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::OAUTH_SERVER_ERROR, error.status);
  EXPECT_FALSE(token);
  EXPECT_TRUE(network_wrapper_.GetRequest());
}

TEST_F(ServiceAccountTokenProviderTest, ResponseFormatError) {
  network_wrapper_.SetResponse(GetResponse(nullptr, 200, ""));

  fuchsia::modular::auth::FirebaseTokenPtr token;
  fuchsia::modular::auth::AuthErr error;

  ASSERT_TRUE(GetToken("api_key", &token, &error));
  EXPECT_EQ(fuchsia::modular::auth::Status::BAD_RESPONSE, error.status);
  EXPECT_FALSE(token);
  EXPECT_TRUE(network_wrapper_.GetRequest());
}

}  // namespace
}  // namespace service_account
