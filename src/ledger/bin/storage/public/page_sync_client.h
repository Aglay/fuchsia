// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_PUBLIC_PAGE_SYNC_CLIENT_H_
#define SRC_LEDGER_BIN_STORAGE_PUBLIC_PAGE_SYNC_CLIENT_H_

#include <src/lib/fxl/macros.h>

#include "src/ledger/bin/storage/public/page_sync_delegate.h"

namespace storage {

// |PageSyncClient| represents the communication interface between storage and
// the synchronization objects.
class PageSyncClient {
 public:
  PageSyncClient() {}
  virtual ~PageSyncClient() {}

  // Sets the PageSyncDelegate for this page. A nullptr can be passed to unset a
  // previously set value.
  virtual void SetSyncDelegate(PageSyncDelegate* page_sync) = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageSyncClient);
};

}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_PUBLIC_PAGE_SYNC_CLIENT_H_
