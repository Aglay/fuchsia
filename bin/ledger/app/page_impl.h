// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_PAGE_IMPL_H_
#define PERIDOT_BIN_LEDGER_APP_PAGE_IMPL_H_

#include "lib/fxl/macros.h"
#include <fuchsia/cpp/ledger.h>

namespace ledger {
class PageDelegate;

// An implementation of the |Page| FIDL interface.
class PageImpl : public Page {
 public:
  explicit PageImpl(PageDelegate* delegate);
  ~PageImpl() override;

 private:
  // Page:
  void GetId(const GetIdCallback& callback) override;

  void GetSnapshot(fidl::InterfaceRequest<PageSnapshot> snapshot_request,
                   fidl::VectorPtr<uint8_t> key_prefix,
                   fidl::InterfaceHandle<PageWatcher> watcher,
                   const GetSnapshotCallback& callback) override;

  void Put(fidl::VectorPtr<uint8_t> key,
           fidl::VectorPtr<uint8_t> value,
           const PutCallback& callback) override;

  void PutWithPriority(fidl::VectorPtr<uint8_t> key,
                       fidl::VectorPtr<uint8_t> value,
                       Priority priority,
                       const PutWithPriorityCallback& callback) override;

  void PutReference(fidl::VectorPtr<uint8_t> key,
                    ReferencePtr reference,
                    Priority priority,
                    const PutReferenceCallback& callback) override;

  void Delete(fidl::VectorPtr<uint8_t> key,
              const DeleteCallback& callback) override;

  void CreateReferenceFromSocket(
      uint64_t size,
      zx::socket data,
      const CreateReferenceFromSocketCallback& callback) override;

  void CreateReferenceFromVmo(
      fsl::SizedVmoTransportPtr data,
      const CreateReferenceFromVmoCallback& callback) override;

  void StartTransaction(const StartTransactionCallback& callback) override;

  void Commit(const CommitCallback& callback) override;

  void Rollback(const RollbackCallback& callback) override;

  void SetSyncStateWatcher(
      fidl::InterfaceHandle<SyncWatcher> watcher,
      const SetSyncStateWatcherCallback& callback) override;

  void WaitForConflictResolution(
      const WaitForConflictResolutionCallback& callback) override;

  PageDelegate* delegate_;

  FXL_DISALLOW_COPY_AND_ASSIGN(PageImpl);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_PAGE_IMPL_H_
