// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_PAGE_SNAPSHOT_IMPL_H_
#define PERIDOT_BIN_LEDGER_APP_PAGE_SNAPSHOT_IMPL_H_

#include <memory>

#include "lib/fxl/tasks/task_runner.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "peridot/bin/ledger/storage/public/commit.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"

namespace ledger {

// An implementation of the |PageSnapshot| FIDL interface.
class PageSnapshotImpl : public PageSnapshot {
 public:
  PageSnapshotImpl(storage::PageStorage* page_storage,
                   std::unique_ptr<const storage::Commit> commit,
                   std::string key_prefix);
  ~PageSnapshotImpl() override;

 private:
  // PageSnapshot:
  void GetEntries(f1dl::VectorPtr<uint8_t> key_start,
                  f1dl::VectorPtr<uint8_t> token,
                  const GetEntriesCallback& callback) override;
  void GetEntriesInline(f1dl::VectorPtr<uint8_t> key_start,
                        f1dl::VectorPtr<uint8_t> token,
                        const GetEntriesInlineCallback& callback) override;
  void GetKeys(f1dl::VectorPtr<uint8_t> key_start,
               f1dl::VectorPtr<uint8_t> token,
               const GetKeysCallback& callback) override;
  void Get(f1dl::VectorPtr<uint8_t> key, const GetCallback& callback) override;
  void GetInline(f1dl::VectorPtr<uint8_t> key,
                 const GetInlineCallback& callback) override;
  void Fetch(f1dl::VectorPtr<uint8_t> key, const FetchCallback& callback) override;
  void FetchPartial(f1dl::VectorPtr<uint8_t> key,
                    int64_t offset,
                    int64_t max_size,
                    const FetchPartialCallback& callback) override;

  storage::PageStorage* page_storage_;
  std::unique_ptr<const storage::Commit> commit_;
  const std::string key_prefix_;
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_PAGE_SNAPSHOT_IMPL_H_
