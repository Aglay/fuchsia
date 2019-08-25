// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_STORAGE_SESSION_STORAGE_XDR_H_
#define SRC_MODULAR_BIN_SESSIONMGR_STORAGE_SESSION_STORAGE_XDR_H_

#include <fuchsia/modular/internal/cpp/fidl.h>

#include "src/modular/lib/fidl/json_xdr.h"

namespace modular {

extern XdrFilterType<fuchsia::modular::internal::StoryData> XdrStoryData[];

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONMGR_STORAGE_SESSION_STORAGE_XDR_H_
