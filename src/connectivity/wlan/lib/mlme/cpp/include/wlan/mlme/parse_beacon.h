// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PARSE_BEACON_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PARSE_BEACON_H_

#include <fuchsia/wlan/mlme/cpp/fidl.h>

#include <fbl/span.h>
#include <wlan/common/element.h>

namespace wlan {

void ParseBeaconElements(fbl::Span<const uint8_t> ies, uint8_t rx_channel,
                         fuchsia::wlan::mlme::BSSDescription* bss_desc);

// The following functions are visible for testing only
void FillRates(fbl::Span<const SupportedRate> supp_rates,
               fbl::Span<const SupportedRate> ext_supp_rates, ::std::vector<uint8_t>* rates);
std::optional<wlan_channel_bandwidth_t> GetVhtCbw(const VhtOperation& vht_op);
wlan_channel_t DeriveChannel(uint8_t rx_channel, std::optional<uint8_t> dsss_chan,
                             const HtOperation* ht_op,
                             std::optional<wlan_channel_bandwidth_t> vht_cbw);

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PARSE_BEACON_H_
