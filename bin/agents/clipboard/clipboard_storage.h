
// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_AGENTS_CLIPBOARD_CLIPBOARD_STORAGE_H_
#define PERIDOT_BIN_AGENTS_CLIPBOARD_CLIPBOARD_STORAGE_H_

#include "lib/component/fidl/component_context.fidl.h"
#include "peridot/lib/ledger_client/ledger_client.h"
#include "peridot/lib/ledger_client/page_client.h"

namespace modular {

// |ClipboardStorage| manages serialization of clipboard data to and from the
// Ledger.
//
// A Ledger is scoped to a single user, so each user has their own clipboard.
// Using the Ledger for the clipboard means that the same clipboard is shared
// across all of a user's devices. The clipboard will also persist across
// reboots.
class ClipboardStorage : public PageClient {
 public:
  ClipboardStorage(LedgerClient* ledger_client, LedgerPageId page_id);
  ~ClipboardStorage();

  // Stores the provided text.
  void Push(const f1dl::String& text);

  // Returns the most recent value that was passed to |Push()|, or "" if nothing
  // has been pushed yet.
  void Peek(const std::function<void(f1dl::String)>& callback);

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(ClipboardStorage);
};

}  // namespace modular

#endif  // PERIDOT_BIN_AGENTS_CLIPBOARD_CLIPBOARD_STORAGE_H_