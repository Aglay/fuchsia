// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <wlan/common/element.h>

namespace wlan {

struct HtConfig {
    bool ready;
    bool cbw_40_rx_ready;
    bool cbw_40_tx_ready;
};

HtCapabilities BuildHtCapabilities(const HtConfig& config);

HtOperation BuildHtOperation(const wlan_channel_t& chan);

} // namespace wlan