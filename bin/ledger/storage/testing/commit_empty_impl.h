// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_STORAGE_TESTING_COMMIT_EMPTY_IMPL_H_
#define PERIDOT_BIN_LEDGER_STORAGE_TESTING_COMMIT_EMPTY_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include "peridot/bin/ledger/storage/public/commit.h"

namespace storage {

// Empty implementaton of Commit. All methods do nothing and return dummy or
// empty responses.
class CommitEmptyImpl : public Commit {
 public:
  CommitEmptyImpl() = default;
  ~CommitEmptyImpl() override = default;

  // Commit:
  std::unique_ptr<const Commit> Clone() const override;

  const CommitId& GetId() const override;

  std::vector<CommitIdView> GetParentIds() const override;

  int64_t GetTimestamp() const override;

  uint64_t GetGeneration() const override;

  ObjectIdentifier GetRootIdentifier() const override;

  fxl::StringView GetStorageBytes() const override;
};

}  // namespace storage

#endif  // PERIDOT_BIN_LEDGER_STORAGE_TESTING_COMMIT_EMPTY_IMPL_H_
