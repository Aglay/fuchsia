// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/types.h>

namespace board_test {

static constexpr size_t kNameLengthMax = 32;

// Describes metadata passed via ZBI to test board driver.

struct DeviceEntry {
    char name[kNameLengthMax];
    // BIND_PLATFORM_DEV_VID`
    uint32_t vid;
    // BIND_PLATFORM_DEV_PID`
    uint32_t pid;
    // BIND_PLATFORM_DEV_DID`
    uint32_t did;
};

struct DeviceList {
    size_t count;
    DeviceEntry list[];
};

} // namespace board_test
