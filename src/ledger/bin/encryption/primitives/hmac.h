// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_ENCRYPTION_PRIMITIVES_HMAC_H_
#define SRC_LEDGER_BIN_ENCRYPTION_PRIMITIVES_HMAC_H_

#include "src/lib/fxl/strings/string_view.h"

namespace encryption {
// Compute the HMAC defined by RFC 2104 using SHA-256 for the hash algorithm.
// |key| must be at least 256 bits long.
std::string SHA256HMAC(fxl::StringView key, fxl::StringView data);
}  // namespace encryption

#endif  // SRC_LEDGER_BIN_ENCRYPTION_PRIMITIVES_HMAC_H_
