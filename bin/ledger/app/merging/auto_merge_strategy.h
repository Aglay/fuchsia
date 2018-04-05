// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_MERGING_AUTO_MERGE_STRATEGY_H_
#define PERIDOT_BIN_LEDGER_APP_MERGING_AUTO_MERGE_STRATEGY_H_

#include <fuchsia/cpp/ledger.h>
#include <memory>
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/app/merging/merge_strategy.h"
#include "peridot/bin/ledger/storage/public/commit.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"

namespace ledger {
// Strategy for merging commits using the AUTOMATIC_WITH_FALLBACK policy.
class AutoMergeStrategy : public MergeStrategy {
 public:
  explicit AutoMergeStrategy(ConflictResolverPtr conflict_resolver);
  ~AutoMergeStrategy() override;

  // MergeStrategy:
  void SetOnError(fxl::Closure on_error) override;

  void Merge(storage::PageStorage* storage,
             PageManager* page_manager,
             std::unique_ptr<const storage::Commit> head_1,
             std::unique_ptr<const storage::Commit> head_2,
             std::unique_ptr<const storage::Commit> ancestor,
             std::function<void(Status)> callback) override;

  void Cancel() override;

 private:
  class AutoMerger;

  fxl::Closure on_error_;

  ConflictResolverPtr conflict_resolver_;

  std::unique_ptr<AutoMerger> in_progress_merge_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AutoMergeStrategy);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_MERGING_AUTO_MERGE_STRATEGY_H_
