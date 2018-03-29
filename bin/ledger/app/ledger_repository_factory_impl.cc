// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/app/ledger_repository_factory_impl.h"

#include <trace/event.h>

#include "garnet/lib/backoff/exponential_backoff.h"
#include "lib/fxl/files/directory.h"
#include "lib/fxl/files/file.h"
#include "lib/fxl/files/path.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/random/rand.h"
#include "lib/fxl/strings/concatenate.h"
#include "lib/fxl/strings/string_view.h"
#include "peridot/bin/ledger/app/constants.h"
#include "peridot/bin/ledger/cloud_sync/impl/user_sync_impl.h"

namespace ledger {

namespace {

constexpr fxl::StringView kContentPath = "/content";
constexpr fxl::StringView kStagingPath = "/staging";

bool GetRepositoryName(fidl::StringPtr repository_path, std::string* name) {
  std::string name_path = repository_path.get() + "/name";

  if (files::ReadFileToString(name_path, name)) {
    return true;
  }

  if (!files::CreateDirectory(repository_path.get())) {
    return false;
  }

  std::string new_name;
  new_name.resize(16);
  fxl::RandBytes(&new_name[0], new_name.size());
  if (!files::WriteFile(name_path, new_name.c_str(), new_name.size())) {
    FXL_LOG(ERROR) << "Unable to write file at: " << name_path;
    return false;
  }

  name->swap(new_name);
  return true;
}

}  // namespace

// Container for a LedgerRepositoryImpl that keeps track of the in-flight FIDL
// requests and callbacks and fires them when the repository is available.
class LedgerRepositoryFactoryImpl::LedgerRepositoryContainer {
 public:
  explicit LedgerRepositoryContainer() {}
  ~LedgerRepositoryContainer() {
    for (const auto& request : requests_) {
      request.second(Status::INTERNAL_ERROR);
    }
  }

  void set_on_empty(const fxl::Closure& on_empty_callback) {
    on_empty_callback_ = on_empty_callback;
    if (ledger_repository_) {
      ledger_repository_->set_on_empty(on_empty_callback);
    }
  };

  // Keeps track of |request| and |callback|. Binds |request| and fires
  // |callback| when the repository is available or an error occurs.
  void BindRepository(fidl::InterfaceRequest<ledger_internal::LedgerRepository> request,
                      std::function<void(Status)> callback) {
    if (status_ != Status::OK) {
      callback(status_);
      return;
    }
    if (ledger_repository_) {
      ledger_repository_->BindRepository(std::move(request));
      callback(status_);
      return;
    }
    requests_.emplace_back(std::move(request), std::move(callback));
  }

  // Sets the implementation or the error status for the container. This
  // notifies all awaiting callbacks and binds all pages in case of success.
  void SetRepository(Status status,
                     std::unique_ptr<LedgerRepositoryImpl> ledger_repository) {
    FXL_DCHECK(!ledger_repository_);
    FXL_DCHECK(status != Status::OK || ledger_repository);
    status_ = status;
    ledger_repository_ = std::move(ledger_repository);
    for (auto& request : requests_) {
      if (ledger_repository_) {
        ledger_repository_->BindRepository(std::move(request.first));
      }
      request.second(status_);
    }
    requests_.clear();
    if (on_empty_callback_) {
      if (ledger_repository_) {
        ledger_repository_->set_on_empty(on_empty_callback_);
      } else {
        on_empty_callback_();
      }
    }
  }

  // Shuts down the repository impl (if already initialized) and detaches all
  // handles bound to it, moving their owneship to the container.
  void Detach() {
    if (ledger_repository_) {
      detached_handles_ = ledger_repository_->Unbind();
      ledger_repository_.reset();
    }
    for (auto& request : requests_) {
      detached_handles_.push_back(std::move(request.first));
    }

    // TODO(ppi): rather than failing all already pending and future requests,
    // we should stash them and fulfill them once the deletion is finished.
    status_ = Status::INTERNAL_ERROR;
  }

 private:
  std::unique_ptr<LedgerRepositoryImpl> ledger_repository_;
  Status status_ = Status::OK;
  std::vector<std::pair<fidl::InterfaceRequest<ledger_internal::LedgerRepository>,
                        std::function<void(Status)>>>
      requests_;
  fxl::Closure on_empty_callback_;
  std::vector<fidl::InterfaceRequest<ledger_internal::LedgerRepository>> detached_handles_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryContainer);
};

struct LedgerRepositoryFactoryImpl::RepositoryInformation {
 public:
  explicit RepositoryInformation(fidl::StringPtr repository_path)
      : base_path(files::SimplifyPath(repository_path.get())),
        content_path(fxl::Concatenate({base_path, kContentPath})),
        staging_path(fxl::Concatenate({base_path, kStagingPath})) {}

  RepositoryInformation(const RepositoryInformation& other) = default;
  RepositoryInformation(RepositoryInformation&& other) = default;

  bool Init() { return GetRepositoryName(content_path, &name); }

  const std::string base_path;
  const std::string content_path;
  const std::string staging_path;
  std::string name;
};

LedgerRepositoryFactoryImpl::LedgerRepositoryFactoryImpl(
    ledger::Environment* environment)
    : environment_(environment) {}

LedgerRepositoryFactoryImpl::~LedgerRepositoryFactoryImpl() {}

void LedgerRepositoryFactoryImpl::GetRepository(
    fidl::StringPtr repository_path,
    fidl::InterfaceHandle<cloud_provider::CloudProvider> cloud_provider,
    fidl::InterfaceRequest<ledger_internal::LedgerRepository> repository_request,
    GetRepositoryCallback callback) {
  TRACE_DURATION("ledger", "repository_factory_get_repository");
  RepositoryInformation repository_information(repository_path);
  if (!repository_information.Init()) {
    callback(Status::IO_ERROR);
    return;
  }
  auto it = repositories_.find(repository_information.name);
  if (it != repositories_.end()) {
    it->second.BindRepository(std::move(repository_request), callback);
    return;
  }

  if (!cloud_provider) {
    FXL_LOG(WARNING) << "No cloud provider - Ledger will work locally but "
                     << "not sync. (running in Guest mode?)";

    auto ret = repositories_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(repository_information.name),
        std::forward_as_tuple());
    LedgerRepositoryContainer* container = &ret.first->second;
    container->BindRepository(std::move(repository_request), callback);
    std::unique_ptr<SyncWatcherSet> watchers =
        std::make_unique<SyncWatcherSet>();
    auto repository = std::make_unique<LedgerRepositoryImpl>(
        repository_information.content_path, environment_, std::move(watchers),
        nullptr);
    container->SetRepository(Status::OK, std::move(repository));
    return;
  }

  auto cloud_provider_ptr = cloud_provider.Bind();
  cloud_provider_ptr.set_error_handler(
      [this, name = repository_information.name] {
        FXL_LOG(ERROR) << "Lost connection to the cloud provider, "
                       << "shutting down the repository.";
        auto find_repository = repositories_.find(name);
        FXL_DCHECK(find_repository != repositories_.end());
        repositories_.erase(find_repository);
      });

  auto ret =
      repositories_.emplace(std::piecewise_construct,
                            std::forward_as_tuple(repository_information.name),
                            std::forward_as_tuple());
  LedgerRepositoryContainer* container = &ret.first->second;
  container->BindRepository(std::move(repository_request), callback);

  cloud_sync::UserConfig user_config;
  user_config.user_directory = repository_information.content_path;
  user_config.cloud_provider = std::move(cloud_provider_ptr);
  CreateRepository(container, repository_information, std::move(user_config));
}

void LedgerRepositoryFactoryImpl::CreateRepository(
    LedgerRepositoryContainer* container,
    const RepositoryInformation& repository_information,
    cloud_sync::UserConfig user_config) {
  std::unique_ptr<SyncWatcherSet> watchers = std::make_unique<SyncWatcherSet>();
  fxl::Closure on_version_mismatch = [this, repository_information]() mutable {
    OnVersionMismatch(repository_information);
  };
  auto user_sync = std::make_unique<cloud_sync::UserSyncImpl>(
      environment_, std::move(user_config),
      std::make_unique<backoff::ExponentialBackoff>(), watchers.get(),
      std::move(on_version_mismatch));
  user_sync->Start();
  auto repository = std::make_unique<LedgerRepositoryImpl>(
      repository_information.content_path, environment_, std::move(watchers),
      std::move(user_sync));
  container->SetRepository(Status::OK, std::move(repository));
}

void LedgerRepositoryFactoryImpl::OnVersionMismatch(
    RepositoryInformation repository_information) {
  FXL_LOG(WARNING)
      << "Data in the cloud was wiped out, erasing local state. "
      << "This should log you out, log back in to start syncing again.";

  // First, shut down the repository so that we can delete the files while it's
  // not running.
  auto find_repository = repositories_.find(repository_information.name);
  FXL_DCHECK(find_repository != repositories_.end());
  find_repository->second.Detach();
  DeleteRepositoryDirectory(repository_information);
  repositories_.erase(find_repository);
}

Status LedgerRepositoryFactoryImpl::DeleteRepositoryDirectory(
    const RepositoryInformation& repository_information) {
  files::ScopedTempDir tmp_directory(repository_information.staging_path);
  std::string destination = tmp_directory.path() + "/content";

  if (rename(repository_information.content_path.c_str(),
             destination.c_str()) != 0) {
    FXL_LOG(ERROR) << "Unable to move repository local storage at "
                   << repository_information.content_path << " to "
                   << destination << ". Error: " << strerror(errno);
    return Status::IO_ERROR;
  }
  if (!files::DeletePath(destination, true)) {
    FXL_LOG(ERROR) << "Unable to delete repository staging storage at "
                   << destination;
    return Status::IO_ERROR;
  }
  return Status::OK;
}

}  // namespace ledger
