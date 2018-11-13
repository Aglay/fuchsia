// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <string>

#include "garnet/lib/debug_ipc/helper/zx_status_definitions.h"

namespace debug_ipc {

std::string ZxStatusToString(uint32_t status);

}  // namespace debug_ipc

