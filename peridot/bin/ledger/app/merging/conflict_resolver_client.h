// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_MERGING_CONFLICT_RESOLVER_CLIENT_H_
#define PERIDOT_BIN_LEDGER_APP_MERGING_CONFLICT_RESOLVER_CLIENT_H_

#include <memory>
#include <vector>

#include <lib/callback/operation_serializer.h>
#include <lib/callback/waiter.h>
#include <lib/fit/function.h>
#include <lib/fxl/macros.h>
#include <lib/fxl/memory/weak_ptr.h>

#include "peridot/bin/ledger/app/diff_utils.h"
#include "peridot/bin/ledger/app/page_manager.h"
#include "peridot/bin/ledger/fidl/error_notifier.h"
#include "peridot/bin/ledger/fidl/include/types.h"
#include "peridot/bin/ledger/storage/public/commit.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"

namespace ledger {
// Client handling communication with a ConflictResolver interface in order to
// merge conflicting commit branches. It is used both by AutoMergeStrategy and
// CustomMergeStrategy.
class ConflictResolverClient
    : public fuchsia::ledger::MergeResultProviderErrorNotifierDelegate {
 public:
  explicit ConflictResolverClient(
      storage::PageStorage* storage, PageManager* page_manager,
      ConflictResolver* conflict_resolver,
      std::unique_ptr<const storage::Commit> left,
      std::unique_ptr<const storage::Commit> right,
      std::unique_ptr<const storage::Commit> ancestor,
      fit::function<void(Status)> callback);
  ~ConflictResolverClient() override;

  void Start();
  void Cancel();

 private:
  void OnNextMergeResult(
      const MergedValue& merged_value,
      const fxl::RefPtr<callback::Waiter<storage::Status,
                                         storage::ObjectIdentifier>>& waiter);

  // Rolls back journal, closes merge result provider and invokes callback_ with
  // |status|. This method must be called at most once.
  void Finalize(Status status);

  // Performs a diff of the given type on the conflict. Return a |Status|
  // different than OK if an error occured. If no error, return an
  // |IterationStatus|.
  void GetDiff(
      diff_utils::DiffType type, std::unique_ptr<Token> token,
      fit::function<void(Status, IterationStatus, std::vector<DiffEntry>,
                         std::unique_ptr<Token>)>
          callback);

  // MergeResultProviderNotifierDelegate:
  void GetFullDiff(std::unique_ptr<Token> token,
                   fit::function<void(Status, Status, std::vector<DiffEntry>,
                                      std::unique_ptr<Token>)>
                       callback) override;
  void GetFullDiffNew(
      std::unique_ptr<Token> token,
      fit::function<void(Status, IterationStatus, std::vector<DiffEntry>,
                         std::unique_ptr<Token>)>
          callback) override;
  void GetConflictingDiff(
      std::unique_ptr<Token> token,
      fit::function<void(Status, Status, std::vector<DiffEntry>,
                         std::unique_ptr<Token>)>
          callback) override;
  void GetConflictingDiffNew(
      std::unique_ptr<Token> token,
      fit::function<void(Status, IterationStatus, std::vector<DiffEntry>,
                         std::unique_ptr<Token>)>
          callback) override;
  void Merge(std::vector<MergedValue> merged_values,
             fit::function<void(Status, Status)> callback) override;
  void MergeNew(std::vector<MergedValue> merged_values,
                fit::function<void(Status)> callback) override;
  void MergeNonConflictingEntries(
      fit::function<void(Status, Status)> callback) override;
  void MergeNonConflictingEntriesNew(
      fit::function<void(Status)> callback) override;
  void Done(fit::function<void(Status, Status)> callback) override;
  void DoneNew(fit::function<void(Status)> callback) override;

  // Checks whether this ConflictResolverClient is still valid (not deleted nor
  // cancelled) and the status is OK. Returns |true| in that case. Otherwise,
  // calls |callback| with the given |status| and calls |Finalize| if this
  // object is not deleted, then return |false|.
  static bool IsInValidStateAndNotify(
      const fxl::WeakPtr<ConflictResolverClient>& weak_this,
      const fit::function<void(Status)>& callback,
      storage::Status status = storage::Status::OK);

  storage::PageStorage* const storage_;
  PageManager* const manager_;
  ConflictResolver* const conflict_resolver_;

  std::unique_ptr<const storage::Commit> const left_;
  std::unique_ptr<const storage::Commit> const right_;
  std::unique_ptr<const storage::Commit> const ancestor_;

  // Called when the merge process is finished.
  fit::function<void(Status)> callback_;

  // |has_merged_values_| is true when |Merge| has been called to set some
  // values. It is used as an optimization in |MergeNonConflictingEntries|.
  bool has_merged_values_ = false;
  std::unique_ptr<storage::Journal> journal_;
  // |in_client_request_| is true when waiting for the callback of the
  // ConflictResolver.Resolve call. When this merge is cancelled, we check this
  // boolean to know if we should abort immediately (when in a client request,
  // as the client may have disconnected) and when we should wait for the
  // operation to finish (the other cases, such as committing the merge).
  bool in_client_request_ = false;
  bool cancelled_ = false;

  // Operations are operating on the state of the merge commit. They must be
  // serialized.
  callback::OperationSerializer operation_serializer_;

  ledger::ErrorNotifierBinding<
      fuchsia::ledger::MergeResultProviderErrorNotifierDelegate>
      merge_result_provider_binding_;

  // This must be the last member of the class.
  fxl::WeakPtrFactory<ConflictResolverClient> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ConflictResolverClient);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_MERGING_CONFLICT_RESOLVER_CLIENT_H_
