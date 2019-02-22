// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_WLAN_MLME_INCLUDE_WLAN_MLME_CONVERT_H_
#define GARNET_LIB_WLAN_MLME_INCLUDE_WLAN_MLME_CONVERT_H_

#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <garnet/lib/rust/wlan-mlme-c/bindings.h>
#include <wlan/common/mac_frame.h>

namespace wlan {

wlan_status_code ToStatusCode(const ::fuchsia::wlan::mlme::AuthenticateResultCodes code);
wlan_status_code ToStatusCode(const ::fuchsia::wlan::mlme::AssociateResultCodes code);

}  // namespace wlan

#endif  // GARNET_LIB_WLAN_MLME_INCLUDE_WLAN_MLME_CONVERT_H_
