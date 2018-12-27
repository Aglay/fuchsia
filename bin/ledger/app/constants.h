// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_CONSTANTS_H_
#define PERIDOT_BIN_LEDGER_APP_CONSTANTS_H_

#include <lib/fxl/strings/string_view.h>

#include "peridot/bin/ledger/storage/public/types.h"

namespace ledger {

// The maximum key size.
inline constexpr size_t kMaxKeySize = 256;

// The root Page ID.
extern const fxl::StringView kRootPageId;

// Filename under which the server id used to sync a given user is stored within
// the repository dir of that user.
inline constexpr fxl::StringView kServerIdFilename = "server_id";

// The serialization version of PageUsage DB.
inline constexpr fxl::StringView kPageUsageDbSerializationVersion = "1";

inline constexpr char kRepositoriesInspectPathComponent[] = "repositories";
inline constexpr char kRequestsInspectPathComponent[] = "requests";
inline constexpr char kLedgersInspectPathComponent[] = "ledgers";

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_CONSTANTS_H_
