// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_DEVICE_RUNNER_USER_PROVIDER_IMPL_H_
#define PERIDOT_BIN_DEVICE_RUNNER_USER_PROVIDER_IMPL_H_

#include <fuchsia/auth/cpp/fidl.h>
#include <fuchsia/modular/auth/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async/cpp/future.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>

#include "peridot/bin/device_runner/user_controller_impl.h"

namespace fuchsia {
namespace modular {
struct UsersStorage;
}
}  // namespace fuchsia

namespace modular {

class UserProviderImpl : fuchsia::modular::UserProvider,
                         fuchsia::auth::AuthenticationContextProvider {
 public:
  // Users of UserProviderImpl must register a Delegate object.
  class Delegate {
   public:
    // Called after UserProviderImpl successfully logs in a user.
    virtual void DidLogin() = 0;

    // Called after UserProviderImpl successfully logs out a user.
    virtual void DidLogout() = 0;

    // Enables the delegate to intercept the user shell's view owner, so that
    // e.g. the delegate can embed it in a parent view or present it.
    // |default_view_owner| is the view owner request that's passed to
    // UserProviderImpl from device shell. If you don't need to intercept the
    // view owner, return it without modifying it.
    virtual fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
    GetUserShellViewOwner(
        fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
            default_view_owner) = 0;

    // Enables the delegate to supply a different service provider to the user
    // shell. |default_service_provider| is the service provider passed to the
    // user shell by the device shell. If you don't need to replace it, return
    // it without modifying it.
    virtual fidl::InterfaceHandle<fuchsia::sys::ServiceProvider>
    GetUserShellServiceProvider(
        fidl::InterfaceHandle<fuchsia::sys::ServiceProvider>
            default_service_provider) = 0;
  };

  // |account_provider| and |delegate| must outlive UserProviderImpl.
  UserProviderImpl(std::shared_ptr<component::StartupContext> context,
                   const fuchsia::modular::AppConfig& user_runner,
                   const fuchsia::modular::AppConfig& default_user_shell,
                   const fuchsia::modular::AppConfig& story_shell,
                   fuchsia::modular::auth::AccountProvider* account_provider,
                   fuchsia::auth::TokenManagerFactory* token_manager_factory,
                   bool use_token_manager_factory, Delegate* const delegate);

  void Connect(fidl::InterfaceRequest<fuchsia::modular::UserProvider> request);

  void Teardown(const std::function<void()>& callback);

  // Stops the active user shell, and starts the user shell specified in
  // |user_shell_config|. This has no effect, and will return an
  // immediately-completed future, if no user shells are running.
  FuturePtr<> SwapUserShell(fuchsia::modular::AppConfig user_shell_config);

 private:
  // |fuchsia::modular::UserProvider|
  void Login(fuchsia::modular::UserLoginParams params) override;

  // |fuchsia::modular::UserProvider|
  void PreviousUsers(PreviousUsersCallback callback) override;

  // |fuchsia::modular::UserProvider|
  void AddUser(fuchsia::modular::auth::IdentityProvider identity_provider,
               AddUserCallback callback) override;

  // |fuchsia::modular::UserProvider|
  void RemoveUser(fidl::StringPtr account_id,
                  RemoveUserCallback callback) override;

  // |fuchsia::auth::AuthenticationContextProvider|
  void GetAuthenticationUIContext(
      fidl::InterfaceRequest<fuchsia::auth::AuthenticationUIContext> request)
      override;

  // Add user using |fuchsia::modular::auth::AccountProvider| interface.
  void AddUserV1(
      const fuchsia::modular::auth::IdentityProvider identity_provider,
      AddUserCallback callback);

  // Add user using |fuchsia::auth::TokenManagerFactory| interface.
  void AddUserV2(
      const fuchsia::modular::auth::IdentityProvider identity_provider,
      AddUserCallback callback);

  // Remove user using |fuchsia::modular::auth::AccountProvider| interface.
  void RemoveUserV1(fuchsia::modular::auth::AccountPtr account,
                    RemoveUserCallback callback);

  // Remove user using |fuchsia::auth::TokenManagerFactory| interface.
  void RemoveUserV2(fuchsia::modular::auth::AccountPtr account,
                    RemoveUserCallback callback);

  bool AddUserToAccountsDB(fuchsia::modular::auth::AccountPtr account,
                           std::string* error);
  bool RemoveUserFromAccountsDB(fidl::StringPtr account_id, std::string* error);
  bool WriteUsersDb(const std::string& serialized_users, std::string* error);
  bool Parse(const std::string& serialized_users);

  void LoginInternal(fuchsia::modular::auth::AccountPtr account,
                     fuchsia::modular::UserLoginParams params);

  fidl::BindingSet<fuchsia::modular::UserProvider> bindings_;

  std::shared_ptr<component::StartupContext> context_;
  const fuchsia::modular::AppConfig& user_runner_;  // Neither owned nor copied.
  const fuchsia::modular::AppConfig&
      default_user_shell_;                          // Neither owned nor copied.
  const fuchsia::modular::AppConfig& story_shell_;  // Neither owned nor copied.
  fuchsia::modular::auth::AccountProvider* const
      account_provider_;  // Neither owned nor copied.
  fuchsia::auth::TokenManagerFactory* const
      token_manager_factory_;  // Neither owned nor copied.
  bool use_token_manager_factory_ = false;
  Delegate* const delegate_;   // Neither owned nor copied.
  fidl::Binding<fuchsia::auth::AuthenticationContextProvider>
      auth_context_provider_binding_;

  std::string serialized_users_;
  const fuchsia::modular::UsersStorage* users_storage_ = nullptr;

  std::map<UserControllerImpl*, std::unique_ptr<UserControllerImpl>>
      user_controllers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UserProviderImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_DEVICE_RUNNER_USER_PROVIDER_IMPL_H_
