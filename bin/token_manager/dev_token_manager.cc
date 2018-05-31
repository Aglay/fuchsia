// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <trace-provider/provider.h>

#include <modular_auth/cpp/fidl.h>
#include "lib/app/cpp/connect.h"
#include "lib/app/cpp/startup_context.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fidl/cpp/string.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/log_settings_command_line.h"
#include "lib/fxl/macros.h"

namespace modular_auth {

class AccountProviderImpl : AccountProvider {
 public:
  AccountProviderImpl(async::Loop* loop);

 private:
  // |AccountProvider| implementation:
  void Initialize(fidl::InterfaceHandle<modular_auth::AccountProviderContext>
                      provider) override;
  void Terminate() override;
  void AddAccount(modular_auth::IdentityProvider identity_provider,
                  AddAccountCallback callback) override;
  void RemoveAccount(modular_auth::Account account, bool revoke_all,
                     RemoveAccountCallback callback) override;
  void GetTokenProviderFactory(
      fidl::StringPtr account_id,
      fidl::InterfaceRequest<modular_auth::TokenProviderFactory> request)
      override;

  std::string GenerateAccountId();

  async::Loop* const loop_;
  std::shared_ptr<component::StartupContext> startup_context_;
  modular_auth::AccountProviderContextPtr account_provider_context_;
  fidl::Binding<AccountProvider> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AccountProviderImpl);
};

AccountProviderImpl::AccountProviderImpl(async::Loop* loop)
    : loop_(loop),
      startup_context_(component::StartupContext::CreateFromStartupInfo()),
      binding_(this) {
  FXL_DCHECK(loop);
  startup_context_->outgoing().AddPublicService<AccountProvider>(
      [this](fidl::InterfaceRequest<AccountProvider> request) {
        binding_.Bind(std::move(request));
      });
}

void AccountProviderImpl::Initialize(
    fidl::InterfaceHandle<modular_auth::AccountProviderContext> provider) {
  account_provider_context_.Bind(std::move(provider));
}

void AccountProviderImpl::Terminate() { loop_->Quit(); }

std::string AccountProviderImpl::GenerateAccountId() {
  uint32_t random_number;
  size_t random_size;
  zx_status_t status =
      zx_cprng_draw(&random_number, sizeof random_number, &random_size);
  FXL_CHECK(status == ZX_OK);
  FXL_CHECK(sizeof random_number == random_size);
  return std::to_string(random_number);
}

void AccountProviderImpl::AddAccount(
    modular_auth::IdentityProvider identity_provider,
    AddAccountCallback callback) {
  auto account = modular_auth::Account::New();
  account->id = GenerateAccountId();
  account->identity_provider = identity_provider;
  account->display_name = "";
  account->url = "";
  account->image_url = "";

  switch (identity_provider) {
    case modular_auth::IdentityProvider::DEV:
      callback(std::move(account), nullptr);
      return;
    default:
      callback(nullptr, "Unrecognized Identity Provider");
  }
}

void AccountProviderImpl::RemoveAccount(modular_auth::Account account,
                                        bool revoke_all,
                                        RemoveAccountCallback callback) {}

void AccountProviderImpl::GetTokenProviderFactory(
    fidl::StringPtr account_id,
    fidl::InterfaceRequest<modular_auth::TokenProviderFactory> request) {}

}  // namespace modular_auth

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line)) {
    return 1;
  }

  async::Loop loop(&kAsyncLoopConfigMakeDefault);
  trace::TraceProvider trace_provider(loop.async());

  modular_auth::AccountProviderImpl app(&loop);
  loop.Run();
  return 0;
}
