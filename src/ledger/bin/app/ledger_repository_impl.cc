// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/app/ledger_repository_impl.h"

#include <lib/async/cpp/task.h>
#include <lib/trace/event.h>

#include <set>

#include "src/ledger/bin/app/constants.h"
#include "src/ledger/bin/app/db_view_factory.h"
#include "src/ledger/bin/app/page_utils.h"
#include "src/ledger/bin/app/serialization.h"
#include "src/ledger/bin/cloud_sync/impl/ledger_sync_impl.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/p2p_sync/public/ledger_communicator.h"
#include "src/ledger/bin/storage/impl/ledger_storage_impl.h"
#include "src/ledger/bin/sync_coordinator/public/ledger_sync.h"
#include "src/ledger/lib/convert/convert.h"
#include "src/ledger/lib/coroutine/coroutine.h"
#include "src/ledger/lib/logging/logging.h"
#include "third_party/abseil-cpp/absl/strings/escaping.h"
#include "third_party/abseil-cpp/absl/strings/string_view.h"

namespace ledger {
namespace {
// Encodes opaque bytes in a way that is usable as a directory name.
std::string GetDirectoryName(absl::string_view bytes) { return absl::WebSafeBase64Escape(bytes); }
}  // namespace

LedgerRepositoryImpl::LedgerRepositoryImpl(
    DetachedPath content_path, Environment* environment,
    std::unique_ptr<storage::DbFactory> db_factory, std::unique_ptr<DbViewFactory> dbview_factory,
    std::unique_ptr<PageUsageDb> db, std::unique_ptr<SyncWatcherSet> watchers,
    std::unique_ptr<sync_coordinator::UserSync> user_sync,
    std::unique_ptr<DiskCleanupManager> disk_cleanup_manager,
    std::unique_ptr<BackgroundSyncManager> background_sync_manager,
    std::vector<PageUsageListener*> page_usage_listeners,
    std::unique_ptr<clocks::DeviceIdManager> device_id_manager)
    : content_path_(std::move(content_path)),
      environment_(environment),
      bindings_(environment->dispatcher()),
      db_factory_(std::move(db_factory)),
      dbview_factory_(std::move(dbview_factory)),
      db_(std::move(db)),
      encryption_service_factory_(environment),
      watchers_(std::move(watchers)),
      user_sync_(std::move(user_sync)),
      page_usage_listeners_(std::move(page_usage_listeners)),
      disk_cleanup_manager_(std::move(disk_cleanup_manager)),
      background_sync_manager_(std::move(background_sync_manager)),
      ledger_managers_(environment_->dispatcher()),
      device_id_manager_(std::move(device_id_manager)),
      coroutine_manager_(environment_->coroutine_service()) {
  bindings_.SetOnDiscardable([this] { CheckDiscardable(); });
  ledger_managers_.SetOnDiscardable([this] { CheckDiscardable(); });
  disk_cleanup_manager_->SetOnDiscardable([this] { CheckDiscardable(); });
  background_sync_manager_->SetOnDiscardable([this] { CheckDiscardable(); });
}

LedgerRepositoryImpl::~LedgerRepositoryImpl() {
  for (auto& binding : bindings_) {
    // |Close()| does not call |binding|'s |on_discardable| callback, so |binding| is
    // not destroyed after this call. This would be a memory leak if we were not
    // in |LedgerRepositoryImpl| destructor: as we are in the destructor,
    // |bindings| will be destroyed at the end of this method, and no leak will
    // happen.
    binding.Close(ZX_OK);
  }
}

void LedgerRepositoryImpl::SetOnDiscardable(fit::closure on_discardable) {
  on_discardable_ = std::move(on_discardable);
}

bool LedgerRepositoryImpl::IsDiscardable() const {
  // Even if the LedgerRepository is closed, it should still serve currently
  // connected Ledgers.
  if (!ledger_managers_.IsDiscardable()) {
    return false;
  }

  // The repository has been forced closed and dependencies are now closed, it
  // can be discarded.
  if (state_ != InternalState::ACTIVE) {
    return true;
  }

  // If the repository has not been forced closed, it can be discarded if all
  // dependencies are discardable.
  return bindings_.IsDiscardable() && disk_cleanup_manager_->IsDiscardable() &&
         background_sync_manager_->IsDiscardable();
}

void LedgerRepositoryImpl::BindRepository(
    fidl::InterfaceRequest<ledger_internal::LedgerRepository> repository_request) {
  bindings_.emplace(this, std::move(repository_request));
}

void LedgerRepositoryImpl::PageIsClosedAndSynced(
    absl::string_view ledger_name, storage::PageIdView page_id,
    fit::function<void(Status, PagePredicateResult)> callback) {
  LedgerManager* ledger_manager;
  Status status = GetLedgerManager(ledger_name, &ledger_manager);
  if (status != Status::OK) {
    callback(status, PagePredicateResult::PAGE_OPENED);
    return;
  }

  LEDGER_DCHECK(ledger_manager);
  // |ledger_manager| can be destructed if empty, or if the
  // |LedgerRepositoryImpl| is destructed. In the second case, the callback
  // should not be called. The first case will not happen before the callback
  // has been called, because the manager is non-empty while a page is tracked.
  ledger_manager->PageIsClosedAndSynced(page_id, std::move(callback));
}

void LedgerRepositoryImpl::PageIsClosedOfflineAndEmpty(
    absl::string_view ledger_name, storage::PageIdView page_id,
    fit::function<void(Status, PagePredicateResult)> callback) {
  LedgerManager* ledger_manager;
  Status status = GetLedgerManager(ledger_name, &ledger_manager);
  if (status != Status::OK) {
    callback(status, PagePredicateResult::PAGE_OPENED);
    return;
  }
  LEDGER_DCHECK(ledger_manager);
  // |ledger_manager| can be destructed if empty, or if the
  // |LedgerRepositoryImpl| is destructed. In the second case, the callback
  // should not be called. The first case will not happen before the callback
  // has been called, because the manager is non-empty while a page is tracked.
  ledger_manager->PageIsClosedOfflineAndEmpty(page_id, std::move(callback));
}

void LedgerRepositoryImpl::DeletePageStorage(absl::string_view ledger_name,
                                             storage::PageIdView page_id,
                                             fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback), [this, page_id, ledger_name = convert::ToString(ledger_name)](
                               coroutine::CoroutineHandler* handler) {
        // We need to increase the DeviceId counter each time a page is created then destroyed.
        // There is no correctness issue with increasing this counter too much. Thus, we increase
        // the counter each time a page is evicted/deleted locally. We have to do it before the page
        // is actually deleted otherwise we risk being interrupted in the middle and not actually
        // increase the counter.
        RETURN_ON_ERROR(device_id_manager_->OnPageDeleted(handler));

        LedgerManager* ledger_manager;
        Status status = GetLedgerManager(ledger_name, &ledger_manager);
        if (status != Status::OK) {
          return status;
        }
        LEDGER_DCHECK(ledger_manager);

        if (coroutine::SyncCall(
                handler,
                [ledger_manager, page_id](fit::function<void(Status)> sync_callback) {
                  ledger_manager->DeletePageStorage(page_id, std::move(sync_callback));
                },
                &status) != coroutine::ContinuationStatus::OK) {
          return Status::INTERRUPTED;
        }
        return status;
      });
}

void LedgerRepositoryImpl::TrySyncClosedPage(absl::string_view ledger_name,
                                             storage::PageIdView page_id) {
  LedgerManager* ledger_manager;
  Status status = GetLedgerManager(ledger_name, &ledger_manager);
  if (status != Status::OK) {
    return;
  }
  LEDGER_DCHECK(ledger_manager);
  return ledger_manager->TrySyncClosedPage(page_id);
}

Status LedgerRepositoryImpl::GetLedgerManager(convert::ExtendedStringView ledger_name,
                                              LedgerManager** ledger_manager) {
  LEDGER_DCHECK(!ledger_name.empty());

  // If the Ledger instance is already open return it directly.
  auto it = ledger_managers_.find(ledger_name);
  if (it != ledger_managers_.end()) {
    *ledger_manager = &(it->second);
    return Status::OK;
  }

  std::string name_as_string = convert::ToString(ledger_name);
  std::unique_ptr<encryption::EncryptionService> encryption_service =
      encryption_service_factory_.MakeEncryptionService(name_as_string);
  std::unique_ptr<sync_coordinator::LedgerSync> ledger_sync;
  storage::CommitPruningPolicy pruning_policy;
  if (user_sync_) {
    ledger_sync = user_sync_->CreateLedgerSync(name_as_string, encryption_service.get());
    pruning_policy = storage::CommitPruningPolicy::NEVER;
  } else {
    pruning_policy = storage::CommitPruningPolicy::LOCAL_IMMEDIATE;
  }
  auto ledger_storage = std::make_unique<storage::LedgerStorageImpl>(
      environment_, encryption_service.get(), db_factory_.get(), GetPathFor(name_as_string),
      pruning_policy, device_id_manager_.get());
  RETURN_ON_ERROR(ledger_storage->Init());
  auto result = ledger_managers_.try_emplace(
      name_as_string, environment_, name_as_string, std::move(encryption_service),
      std::move(ledger_storage), std::move(ledger_sync), page_usage_listeners_);
  LEDGER_DCHECK(result.second);
  *ledger_manager = &(result.first->second);
  return Status::OK;
}

void LedgerRepositoryImpl::GetLedger(std::vector<uint8_t> ledger_name,
                                     fidl::InterfaceRequest<Ledger> ledger_request,
                                     fit::function<void(Status)> callback) {
  TRACE_DURATION("ledger", "repository_get_ledger");

  if (state_ != InternalState::ACTIVE) {
    // Attempting to call a method on LedgerRepository while closing it is
    // illegal.
    callback(Status::ILLEGAL_STATE);
    return;
  }

  if (ledger_name.empty()) {
    callback(Status::INVALID_ARGUMENT);
    return;
  }

  LedgerManager* ledger_manager;
  Status status = GetLedgerManager(ledger_name, &ledger_manager);
  if (status != Status::OK) {
    callback(status);
    return;
  }
  LEDGER_DCHECK(ledger_manager);
  ledger_manager->BindLedger(std::move(ledger_request));
  callback(Status::OK);
}

void LedgerRepositoryImpl::Duplicate(
    fidl::InterfaceRequest<ledger_internal::LedgerRepository> request,
    fit::function<void(Status)> callback) {
  if (state_ != InternalState::ACTIVE) {
    // Attempting to call a method on LedgerRepository while closing it is
    // illegal.
    callback(Status::ILLEGAL_STATE);
    return;
  }

  BindRepository(std::move(request));
  callback(Status::OK);
}

void LedgerRepositoryImpl::SetSyncStateWatcher(fidl::InterfaceHandle<SyncWatcher> watcher,
                                               fit::function<void(Status)> callback) {
  if (state_ != InternalState::ACTIVE) {
    // Attempting to call a method on LedgerRepository while closing it is
    // illegal.
    callback(Status::ILLEGAL_STATE);
    return;
  }

  watchers_->AddSyncWatcher(std::move(watcher));
  callback(Status::OK);
}

void LedgerRepositoryImpl::CheckDiscardable() {
  if (!IsDiscardable()) {
    return;
  }

  state_ = InternalState::CLOSED;

  if (on_discardable_) {
    on_discardable_();
  }

  auto callbacks = std::move(close_callbacks_);
  close_callbacks_.clear();
  for (auto& callback : callbacks) {
    callback(Status::OK);
  }
}

void LedgerRepositoryImpl::DiskCleanUp(fit::function<void(Status)> callback) {
  if (state_ != InternalState::ACTIVE) {
    // Attempting to call a method on LedgerRepository while closing it is
    // illegal.
    callback(Status::ILLEGAL_STATE);
    return;
  }

  cleanup_callbacks_.push_back(std::move(callback));
  if (cleanup_callbacks_.size() > 1) {
    return;
  }
  disk_cleanup_manager_->TryCleanUp([this](Status status) {
    LEDGER_DCHECK(!cleanup_callbacks_.empty());

    auto callbacks = std::move(cleanup_callbacks_);
    cleanup_callbacks_.clear();
    for (auto& callback : callbacks) {
      callback(status);
    }
  });
}

DetachedPath LedgerRepositoryImpl::GetPathFor(absl::string_view ledger_name) {
  LEDGER_DCHECK(!ledger_name.empty());
  return content_path_.SubPath(GetDirectoryName(ledger_name));
}

void LedgerRepositoryImpl::Close(fit::function<void(Status)> callback) {
  if (state_ == InternalState::CLOSED) {
    // Closing the repository.
    callback(Status::OK);
    return;
  }
  close_callbacks_.push_back(std::move(callback));

  state_ = InternalState::CLOSING;
  CheckDiscardable();
}

}  // namespace ledger
