// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_APP_LEDGER_IMPL_H_
#define SRC_LEDGER_BIN_APP_LEDGER_IMPL_H_

#include <memory>

#include <lib/fit/function.h>
#include <lib/fxl/macros.h>
#include <lib/fxl/strings/string_view.h>

#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/app/page_manager.h"
#include "src/ledger/bin/environment/environment.h"
#include "src/ledger/bin/fidl/error_notifier.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/storage/public/ledger_storage.h"

namespace ledger {

// An implementation of the |Ledger| FIDL interface.
class LedgerImpl : public fuchsia::ledger::LedgerErrorNotifierDelegate {
 public:
  // Delegate capable of actually performing the page operations.
  class Delegate {
   public:
    // State of a new page. If the state is |NEW|, it is known that it doesn't
    // have any content on the cloud or on another device.
    enum class PageState {
      // The page is new and has been created locally.
      NEW,
      // The page has been named by the client.
      // well known name
      NAMED,
    };

    Delegate() {}
    virtual ~Delegate() = default;

    virtual void GetPage(convert::ExtendedStringView page_id,
                         PageState page_state,
                         fidl::InterfaceRequest<Page> page_request,
                         fit::function<void(storage::Status)> callback) = 0;

    virtual void SetConflictResolverFactory(
        fidl::InterfaceHandle<ConflictResolverFactory> factory) = 0;

   private:
    FXL_DISALLOW_COPY_AND_ASSIGN(Delegate);
  };

  // |delegate| outlives LedgerImpl.
  explicit LedgerImpl(Environment* environment, Delegate* delegate);
  ~LedgerImpl() override;

 private:
  // Ledger:
  void GetRootPage(fidl::InterfaceRequest<Page> page_request,
                      fit::function<void(Status)> callback) override;
  void GetRootPageNew(fidl::InterfaceRequest<Page> page_request,
                      fit::function<void(Status)> callback) override;
  void GetPage(PageIdPtr id, fidl::InterfaceRequest<Page> page_request,
                  fit::function<void(Status)> callback) override;
  void GetPageNew(PageIdPtr id, fidl::InterfaceRequest<Page> page_request,
                  fit::function<void(Status)> callback) override;
  void SetConflictResolverFactory(
      fidl::InterfaceHandle<ConflictResolverFactory> factory,
      fit::function<void(Status)> callback) override;
  void SetConflictResolverFactoryNew(
      fidl::InterfaceHandle<ConflictResolverFactory> factory,
      fit::function<void(Status)> callback) override;

  Environment* const environment_;
  Delegate* const delegate_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerImpl);
};
}  // namespace ledger

#endif  // SRC_LEDGER_BIN_APP_LEDGER_IMPL_H_
