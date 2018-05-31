// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_COMMON_XDR_H_
#define PERIDOT_LIB_COMMON_XDR_H_

#include "peridot/lib/fidl/json_xdr.h"

namespace modular_auth {
class Account;
}  // namespace modular_auth

namespace fuchsia {
namespace modular {

extern const XdrFilterType<modular_auth::Account> XdrAccount[];

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_LIB_COMMON_XDR_H_
