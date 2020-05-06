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
#include <map>

#include <ddk/protocol/wlan/info.h>
#include <wlan/common/macaddr.h>
#include <wlan/protocol/ieee80211.h>

#include "sim-frame.h"
#include "sim-sig-loss-model.h"
#include "sim-sta-ifc.h"

namespace wlan::simulation {

// To simulate the physical environment.
//
class Environment {
 public:
  Environment();
  ~Environment() = default;

  // Add a station into the environment.
  void AddStation(StationIfc* sta) { stations_.emplace(std::pair(sta, Location(0, 0))); }

  // Add a station into the environment at specific location.
  void AddStation(StationIfc* sta, int32_t x, int32_t y) {
    stations_.emplace(std::pair(sta, Location(x, y)));
  }

  // Remove a station from the environment.
  void RemoveStation(StationIfc* sta) { stations_.erase(sta); }

  // Change the location of a station in the environment.
  void MoveStation(StationIfc* sta, int32_t x, int32_t y) {
    RemoveStation(sta);
    stations_.emplace(std::pair(sta, Location(x, y)));
  }

  // Begin simulation. Function will return when there are no more events pending.
  void Run();

  // Send a frame into the simulated environment.
  void Tx(const SimFrame* frame, const WlanTxInfo& tx_info, StationIfc* sender);

  // Ask for a future notification, time is relative to current time. If 'id' is non-null, it will
  // be given a unique identifier for reference in future notification-related operations.
  zx_status_t ScheduleNotification(StationIfc* sta, zx::duration delay, void* payload,
                                   uint64_t* id_out = nullptr);

  // Cancel a future notification, return scheduled payload for station to handle
  zx_status_t CancelNotification(StationIfc* sta, uint64_t id, void** payload_out = nullptr);

  // Get simulation absolute time
  zx::time GetTime() { return time_; }

 private:
  static uint64_t event_count_;

  struct EnvironmentEvent {
    uint64_t id;
    zx::time time;  // The absolute time to fire
    StationIfc* requester;
    void* payload;
  };

  // All registered stations
  std::map<StationIfc*, Location> stations_;

  // Current time
  zx::time time_;

  // Future events, sorted by time
  std::list<std::unique_ptr<EnvironmentEvent>> events_;

  // Signal strength loss model
  std::unique_ptr<SignalLossModel> signal_loss_model_;
};

}  // namespace wlan::simulation

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_ENV_H_
