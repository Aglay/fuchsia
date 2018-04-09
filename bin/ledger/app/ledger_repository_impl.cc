// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/ledger_repository_impl.h"

#include <trace/event.h>

#include "peridot/bin/ledger/cloud_sync/impl/ledger_sync_impl.h"
#include "peridot/bin/ledger/p2p_sync/public/ledger_communicator.h"
#include "peridot/bin/ledger/storage/impl/ledger_storage_impl.h"
#include "peridot/bin/ledger/sync_coordinator/public/ledger_sync.h"
#include "peridot/lib/convert/convert.h"

namespace ledger {

LedgerRepositoryImpl::LedgerRepositoryImpl(
    std::string base_storage_dir,
    Environment* environment,
    std::unique_ptr<SyncWatcherSet> watchers,
    std::unique_ptr<sync_coordinator::UserSync> user_sync)
    : base_storage_dir_(std::move(base_storage_dir)),
      environment_(environment),
      encryption_service_factory_(environment->main_runner()),
      watchers_(std::move(watchers)),
      user_sync_(std::move(user_sync)) {
  bindings_.set_empty_set_handler([this] { CheckEmpty(); });
  ledger_managers_.set_on_empty([this] { CheckEmpty(); });
  ledger_repository_debug_bindings_.set_empty_set_handler(
      [this] { CheckEmpty(); });
}

LedgerRepositoryImpl::~LedgerRepositoryImpl() {}

void LedgerRepositoryImpl::BindRepository(
    fidl::InterfaceRequest<ledger_internal::LedgerRepository>
        repository_request) {
  bindings_.AddBinding(this, std::move(repository_request));
}

std::vector<fidl::InterfaceRequest<ledger_internal::LedgerRepository>>
LedgerRepositoryImpl::Unbind() {
  std::vector<fidl::InterfaceRequest<ledger_internal::LedgerRepository>>
      handles;
  for (auto& binding : bindings_.bindings()) {
    handles.push_back(binding->Unbind());
  }
  bindings_.CloseAll();
  return handles;
}

void LedgerRepositoryImpl::GetLedger(
    fidl::VectorPtr<uint8_t> ledger_name,
    fidl::InterfaceRequest<Ledger> ledger_request,
    GetLedgerCallback callback) {
  TRACE_DURATION("ledger", "repository_get_ledger");

  if (ledger_name->empty()) {
    callback(Status::INVALID_ARGUMENT);
    return;
  }

  auto it = ledger_managers_.find(ledger_name);
  if (it == ledger_managers_.end()) {
    std::string name_as_string = convert::ToString(ledger_name);
    std::unique_ptr<encryption::EncryptionService> encryption_service =
        encryption_service_factory_.MakeEncryptionService(name_as_string);
    std::unique_ptr<storage::LedgerStorage> ledger_storage =
        std::make_unique<storage::LedgerStorageImpl>(
            environment_->main_runner(), environment_->coroutine_service(),
            encryption_service.get(), base_storage_dir_, name_as_string);
    std::unique_ptr<sync_coordinator::LedgerSync> ledger_sync;
    if (user_sync_) {
      ledger_sync = user_sync_->CreateLedgerSync(name_as_string,
                                                 encryption_service.get());
    }
    auto result = ledger_managers_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(std::move(name_as_string)),
        std::forward_as_tuple(environment_, std::move(encryption_service),
                              std::move(ledger_storage),
                              std::move(ledger_sync)));
    FXL_DCHECK(result.second);
    it = result.first;
  }

  it->second.BindLedger(std::move(ledger_request));
  callback(Status::OK);
}

void LedgerRepositoryImpl::Duplicate(
    fidl::InterfaceRequest<ledger_internal::LedgerRepository> request,
    DuplicateCallback callback) {
  BindRepository(std::move(request));
  callback(Status::OK);
}

void LedgerRepositoryImpl::SetSyncStateWatcher(
    fidl::InterfaceHandle<SyncWatcher> watcher,
    SetSyncStateWatcherCallback callback) {
  watchers_->AddSyncWatcher(std::move(watcher));
  callback(Status::OK);
}

void LedgerRepositoryImpl::CheckEmpty() {
  if (!on_empty_callback_)
    return;
  if (ledger_managers_.empty() && bindings_.size() == 0 &&
      ledger_repository_debug_bindings_.size() == 0)
    on_empty_callback_();
}

void LedgerRepositoryImpl::GetLedgerRepositoryDebug(
    fidl::InterfaceRequest<ledger_internal::LedgerRepositoryDebug> request,
    GetLedgerRepositoryDebugCallback callback) {
  ledger_repository_debug_bindings_.AddBinding(this, std::move(request));
  callback(Status::OK);
}

void LedgerRepositoryImpl::GetInstancesList(GetInstancesListCallback callback) {
  fidl::VectorPtr<fidl::VectorPtr<uint8_t>> result =
      fidl::VectorPtr<fidl::VectorPtr<uint8_t>>::New(0);
  for (const auto& key_value : ledger_managers_) {
    result.push_back(convert::ToArray(key_value.first));
  }
  callback(std::move(result));
}

void LedgerRepositoryImpl::GetLedgerDebug(
    fidl::VectorPtr<uint8_t> ledger_name,
    fidl::InterfaceRequest<ledger_internal::LedgerDebug> request,
    GetLedgerDebugCallback callback) {
  auto it = ledger_managers_.find(ledger_name);
  if (it == ledger_managers_.end()) {
    callback(Status::KEY_NOT_FOUND);
  } else {
    it->second.BindLedgerDebug(std::move(request));
    callback(Status::OK);
  }
}

}  // namespace ledger
