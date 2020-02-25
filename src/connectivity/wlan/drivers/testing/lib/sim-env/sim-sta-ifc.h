// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_STA_IFC_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_STA_IFC_H_

#include <net/ethernet.h>

#include <ddk/protocol/wlan/info.h>
#include <wlan/common/macaddr.h>

#include "sim-frame.h"

namespace wlan::simulation {

class SimFrame;
class SimManagementFrame;
struct WlanRxInfo;

class StationIfc {
 public:
  // Handler for different frames.
  virtual void Rx(const SimFrame* frame, WlanRxInfo& info) = 0;

  // Receive notification of a simulation event
  virtual void ReceiveNotification(void* payload) = 0;
};

}  // namespace wlan::simulation

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_STA_IFC_H_
