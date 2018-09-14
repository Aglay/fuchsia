// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_PAGE_USAGE_MANAGER_H_
#define PERIDOT_BIN_LEDGER_APP_PAGE_USAGE_MANAGER_H_

#include "peridot/bin/ledger/storage/public/types.h"

namespace ledger {

// A listener on page usage, that receives notifications when a page is  opened
// or closed.
class PageUsageListener {
 public:
  PageUsageListener() {}
  virtual ~PageUsageListener() {}

  // Called when a page connection has been requested. In case of concurrent
  // connections to the same page, this should only be called once, on the first
  // connection.
  virtual void OnPageOpened(fxl::StringView ledger_name,
                            storage::PageIdView page_id) = 0;

  // Called when the connection to a page closes. In case of concurrent
  // connections to the same page, this should only be called once, when the
  // last connection closes.
  // TODO(nellyv): Add argument on whether the page is synced and and cache it.
  virtual void OnPageClosed(fxl::StringView ledger_name,
                            storage::PageIdView page_id) = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageUsageListener);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_PAGE_USAGE_MANAGER_H_
