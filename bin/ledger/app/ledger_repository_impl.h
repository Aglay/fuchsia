// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_LEDGER_REPOSITORY_IMPL_H_
#define PERIDOT_BIN_LEDGER_APP_LEDGER_REPOSITORY_IMPL_H_

#include <fuchsia/cpp/ledger.h>
#include <fuchsia/cpp/ledger_internal.h>
#include <fuchsia/cpp/modular_auth.h>
#include "garnet/lib/callback/auto_cleanable.h"
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fidl/cpp/interface_ptr_set.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/app/ledger_manager.h"
#include "peridot/bin/ledger/app/sync_watcher_set.h"
#include "peridot/bin/ledger/encryption/impl/encryption_service_factory_impl.h"
#include "peridot/bin/ledger/environment/environment.h"
#include "peridot/bin/ledger/p2p_sync/public/user_communicator.h"
#include "peridot/bin/ledger/sync_coordinator/public/user_sync.h"
#include "peridot/lib/convert/convert.h"

namespace ledger {

class LedgerRepositoryImpl : public ledger_internal::LedgerRepository,
                             public ledger_internal::LedgerRepositoryDebug {
 public:
  LedgerRepositoryImpl(std::string base_storage_dir,
                       Environment* environment,
                       std::unique_ptr<SyncWatcherSet> watchers,
                       std::unique_ptr<sync_coordinator::UserSync> user_sync);
  ~LedgerRepositoryImpl() override;

  void set_on_empty(const fxl::Closure& on_empty_callback) {
    on_empty_callback_ = on_empty_callback;
  }

  void BindRepository(fidl::InterfaceRequest<ledger_internal::LedgerRepository>
                          repository_request);

  // Releases all handles bound to this repository impl.
  std::vector<fidl::InterfaceRequest<LedgerRepository>> Unbind();

 private:
  // LedgerRepository:
  void GetLedger(fidl::VectorPtr<uint8_t> ledger_name,
                 fidl::InterfaceRequest<Ledger> ledger_request,
                 GetLedgerCallback callback) override;
  void Duplicate(
      fidl::InterfaceRequest<ledger_internal::LedgerRepository> request,
      DuplicateCallback callback) override;
  void SetSyncStateWatcher(fidl::InterfaceHandle<SyncWatcher> watcher,
                           SetSyncStateWatcherCallback callback) override;

  void GetLedgerRepositoryDebug(
      fidl::InterfaceRequest<ledger_internal::LedgerRepositoryDebug> request,
      GetLedgerRepositoryDebugCallback callback) override;

  void CheckEmpty();

  // LedgerRepositoryDebug:
  void GetInstancesList(GetInstancesListCallback callback) override;

  void GetLedgerDebug(
      fidl::VectorPtr<uint8_t> ledger_name,
      fidl::InterfaceRequest<ledger_internal::LedgerDebug> request,
      GetLedgerDebugCallback callback) override;

  const std::string base_storage_dir_;
  Environment* const environment_;
  encryption::EncryptionServiceFactoryImpl encryption_service_factory_;
  std::unique_ptr<SyncWatcherSet> watchers_;
  std::unique_ptr<sync_coordinator::UserSync> user_sync_;
  callback::AutoCleanableMap<std::string,
                             LedgerManager,
                             convert::StringViewComparator>
      ledger_managers_;
  fidl::BindingSet<ledger_internal::LedgerRepository> bindings_;
  fxl::Closure on_empty_callback_;

  fidl::BindingSet<ledger_internal::LedgerRepositoryDebug>
      ledger_repository_debug_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryImpl);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_LEDGER_REPOSITORY_IMPL_H_
