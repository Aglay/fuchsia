// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "garnet/bin/auth/token_manager/token_manager_impl.h"
#include "garnet/public/lib/auth/fidl/auth_provider_factory.fidl.h"
#include "lib/app/cpp/connect.h"
#include "lib/svc/cpp/services.h"

namespace auth {

using auth::AuthProviderStatus;
using auth::Status;

TokenManagerImpl::TokenManagerImpl(
    app::ApplicationContext* app_context,
    fidl::Array<AuthProviderConfigPtr> auth_provider_configs)
    : token_cache_(kMaxCacheSize) {
  FXL_CHECK(app_context);

  // TODO: Start the auth provider only when someone does a request to it,
  // instead of starting all the configured providers in advance.
  for (auto& config : auth_provider_configs) {
    if (config->url.get().empty()) {
      FXL_LOG(ERROR) << "Auth provider config url is not set.";
      continue;
    }

    auto launch_info = app::ApplicationLaunchInfo::New();
    launch_info->url = config->url;
    app::Services services;
    launch_info->service_request = services.NewRequest();

    app::ApplicationControllerPtr controller;
    app_context->launcher()->CreateApplication(std::move(launch_info),
                                               controller.NewRequest());
    controller.set_error_handler([this, &config] {
      FXL_LOG(INFO) << "Auth provider " << config->url << " disconnected";
      auth_providers_.erase(config->auth_provider_type);
      auth_provider_controllers_.erase(config->auth_provider_type);
      // TODO: Try reconnecting to Auth provider using some back-off mechanism.
    });
    auth_provider_controllers_[config->auth_provider_type] =
        std::move(controller);

    auth::AuthProviderFactoryPtr auth_provider_factory;
    services.ConnectToService(auth_provider_factory.NewRequest());

    auth::AuthProviderPtr auth_provider_ptr;
    auth_provider_factory->GetAuthProvider(
        auth_provider_ptr.NewRequest(), [](auth::AuthProviderStatus status) {
          if (status != auth::AuthProviderStatus::OK) {
            FXL_LOG(ERROR) << "Failed to connect to the auth provider: "
                           << status;
          }
        });
    auth_provider_ptr.set_error_handler([this, &config] {
      FXL_LOG(INFO) << "Auth provider " << config->url << " disconnected";
      auth_providers_.erase(config->auth_provider_type);
      auth_provider_controllers_.erase(config->auth_provider_type);
      // TODO: Try reconnecting to Auth provider using some back-off mechanism.
    });
    auth_providers_[config->auth_provider_type] = std::move(auth_provider_ptr);
  }
}

TokenManagerImpl::~TokenManagerImpl() {}

void TokenManagerImpl::Authorize(
    const auth::AuthProviderType auth_provider_type,
    fidl::InterfaceHandle<auth::AuthenticationUIContext> auth_ui_context,
    const AuthorizeCallback& callback) {
  auto it = auth_providers_.find(auth_provider_type);
  if (it == auth_providers_.end()) {
    callback(Status::AUTH_PROVIDER_SERVICE_UNAVAILABLE, nullptr);
  }

  it->second->GetPersistentCredential(
      std::move(auth_ui_context),
      [this, callback](AuthProviderStatus status, fidl::String credential) {
        if (status != AuthProviderStatus::OK || credential.get().empty()) {
          callback(Status::INTERNAL_ERROR, nullptr);
          return;
        }

        // TODO: Save credential to data store
        callback(Status::OK, nullptr);
        return;
      });
}

void TokenManagerImpl::GetAccessToken(
    const auth::AuthProviderType auth_provider_type,
    const fidl::String& app_client_id,
    fidl::Array<fidl::String> app_scopes,
    const GetAccessTokenCallback& callback) {
  auto it = auth_providers_.find(auth_provider_type);
  if (it == auth_providers_.end()) {
    callback(Status::AUTH_PROVIDER_SERVICE_UNAVAILABLE, nullptr);
  }

  // TODO: Fetch credential from data store
  fidl::String credential = "TODO";
  fidl::String idp_credential_id = "TODO";

  auto cacheKey = GetCacheKey(auth_provider_type, idp_credential_id);
  cache::OAuthTokens tokens;

  if (token_cache_.Get(cacheKey, &tokens) == cache::Status::kOK &&
      tokens.access_token.IsValid()) {
    callback(Status::OK, tokens.access_token.token);
    return;
  }

  it->second->GetAppAccessToken(
      idp_credential_id, app_client_id, std::move(app_scopes),
      [this, callback, cacheKey, &tokens](AuthProviderStatus status,
                                          AuthTokenPtr access_token) {
        std::string access_token_val;
        if (access_token) {
          access_token_val = access_token->token;
        }

        if (status != AuthProviderStatus::OK) {
          callback(Status::AUTH_PROVIDER_SERVER_ERROR, access_token_val);
          return;
        }

        tokens.access_token.expiration_time =
            fxl::TimePoint::Now() +
            fxl::TimeDelta::FromSeconds(access_token->expires_in);
        tokens.access_token.token = access_token_val;

        auto cacheStatus = token_cache_.Put(cacheKey, tokens);
        if (cacheStatus != cache::Status::kOK) {
          // TODO: log error
          callback(Status::OK, access_token_val);
          return;
        }

        callback(Status::OK, std::move(access_token_val));
        return;
      });
}

void TokenManagerImpl::GetIdToken(
    const auth::AuthProviderType auth_provider_type,
    const fidl::String& audience,
    const GetIdTokenCallback& callback) {
  auto it = auth_providers_.find(auth_provider_type);
  if (it == auth_providers_.end()) {
    callback(Status::AUTH_PROVIDER_SERVICE_UNAVAILABLE, nullptr);
  }

  // TODO: Fetch credential from data store
  fidl::String credential = "TODO";
  fidl::String idp_credential_id = "TODO";

  auto cacheKey = GetCacheKey(auth_provider_type, idp_credential_id);
  cache::OAuthTokens tokens;

  if (token_cache_.Get(cacheKey, &tokens) == cache::Status::kOK &&
      tokens.id_token.IsValid()) {
    callback(Status::OK, tokens.id_token.token);
    return;
  }

  it->second->GetAppIdToken(
      credential, audience,
      [this, callback, cacheKey, &tokens](AuthProviderStatus status,
                                          AuthTokenPtr id_token) {
        std::string id_token_val;
        if (id_token) {
          id_token_val = id_token->token;
        }

        if (status != AuthProviderStatus::OK) {
          callback(Status::AUTH_PROVIDER_SERVER_ERROR, id_token_val);
          return;
        }

        tokens.id_token.expiration_time =
            fxl::TimePoint::Now() +
            fxl::TimeDelta::FromSeconds(id_token->expires_in);
        tokens.id_token.token = id_token_val;

        auto cacheStatus = token_cache_.Put(cacheKey, tokens);
        if (cacheStatus != cache::Status::kOK) {
          // TODO: log error
          callback(Status::OK, id_token_val);
          return;
        }

        callback(Status::OK, id_token_val);
        return;
      });
}

void TokenManagerImpl::GetFirebaseToken(
    const auth::AuthProviderType auth_provider_type,
    const fidl::String& firebase_api_key,
    const GetFirebaseTokenCallback& callback) {
  auto it = auth_providers_.find(auth_provider_type);
  if (it == auth_providers_.end()) {
    callback(Status::AUTH_PROVIDER_SERVICE_UNAVAILABLE, nullptr);
  }
  //  TODO: Return from cache if not expired

  // TODO: Fetch fresh id_token
  fidl::String id_token = "TODO";

  it->second->GetAppFirebaseToken(
      id_token, firebase_api_key,
      [this, callback](AuthProviderStatus status, FirebaseTokenPtr fb_token) {
        if (status != AuthProviderStatus::OK) {
          callback(Status::AUTH_PROVIDER_SERVER_ERROR, std::move(fb_token));
          return;
        }

        callback(Status::OK, std::move(fb_token));
        return;
      });
}

void TokenManagerImpl::DeleteAllTokens(
    const auth::AuthProviderType auth_provider_type,
    const DeleteAllTokensCallback& callback) {
  auto it = auth_providers_.find(auth_provider_type);
  if (it == auth_providers_.end()) {
    callback(Status::AUTH_PROVIDER_SERVICE_UNAVAILABLE);
  }

  // TODO: Fetch credential from data store
  fidl::String credential = "TODO";
  fidl::String idp_credential_id = "TODO";
  cache::CacheKey cacheKey = GetCacheKey(auth_provider_type, idp_credential_id);

  it->second->RevokeAppOrPersistentCredential(
      credential, [this, cacheKey, callback](AuthProviderStatus status) {
        if (status != AuthProviderStatus::OK) {
          callback(Status::AUTH_PROVIDER_SERVER_ERROR);
          return;
        }

        auto cacheStatus = token_cache_.Delete(cacheKey);
        if (cacheStatus != cache::Status::kOK &&
            cacheStatus != cache::Status::kKeyNotFound) {
          callback(Status::INTERNAL_CACHE_ERROR);
          return;
        }

        //  TODO: Delete local copy from data store
        callback(Status::OK);
        return;
      });
}

const cache::CacheKey TokenManagerImpl::GetCacheKey(
    auth::AuthProviderType identity_provider,
    const fidl::String& idp_credential_id) {
  // TODO: consider replacing the static cast with a string map (more type safe)
  return cache::CacheKey(std::to_string(static_cast<int>(identity_provider)),
                         idp_credential_id.get());
}

}  // namespace auth
