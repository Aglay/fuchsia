// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The simulated environment of physical world.
//
// In order to support unit-test of the driver code, we need to mimic the
// real-world. See the below diagram, the 'sim-env' module accepts the requests
// from 'unit test' to create a virtual environment and interacts with
// the 'sim-fw', which is a firmware simulation model.
//
//   +-------------+           +------------+
//   |  unit test  | <-------> |   driver   |
//   +-------------+           +------------+
//        ^     \                    ^
//        |        \                 |
//        |           \              |
//        |              \           |
//        v                 \        v
//   +-------------+           +------------+
//   |   sim-env   | <-------> |   sim-fw   |
//   +-------------+           +------------+
//
// In this framework, 'unit test', 'driver' and 'sim-fw' are device-specific
// implementations. 'sim-env' is commonly used for all drivers. Therefore we can
// leverage the fancy features (e.g. RSSI model) in the sim-env for all drivers.
//

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_ENV_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_ENV_H_

#include <lib/zx/time.h>
#include <netinet/if_ether.h>
#include <stdint.h>
#include <zircon/types.h>

#include <list>

#include <wlan/common/macaddr.h>
#include <wlan/protocol/ieee80211.h>
#include <wlan/protocol/info.h>

#include "sim-sta-ifc.h"

namespace wlan::simulation {

// To simulate the physical environment.
//
class Environment {
 public:
  Environment() = default;
  ~Environment() = default;

  // Add a station into the environment.
  void AddStation(StationIfc* sta) { stations_.push_back(sta); }

  // Remove a station from the environment.
  void RemoveStation(StationIfc* sta) { stations_.remove(sta); }

  // Begin simulation. Function will return when there are no more events pending.
  void Run();

  // Send a packet into the simulated environment.
  void Tx();

  // Send a beacon from sender to the rest of the stations. Note that this will (likely) someday
  // be deprecated in favor of Tx().
  void TxBeacon(StationIfc* sender, const wlan_channel_t& chan, const wlan_ssid_t& ssid,
                const common::MacAddr& bssid);

  // Ask for a future notification, time is relative to current time.
  zx_status_t ScheduleNotification(StationIfc* sta, zx::duration delay, void* payload);

  // Get simulation absolute time
  zx::time GetTime() { return time_; }

 private:
  struct EnvironmentEvent {
    zx::time time;  // The absolute time to fire
    StationIfc* requester;
    void* payload;
  };

  // All registered stations
  std::list<StationIfc*> stations_;

  // Current time
  zx::time time_;

  // Future events, sorted by time
  std::list<std::unique_ptr<EnvironmentEvent>> events_;
};

}  // namespace wlan::simulation

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_ENV_H_
