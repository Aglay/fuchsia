// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sim-env.h"

#include <zircon/assert.h>

namespace wlan::simulation {

uint64_t Environment::event_count_ = 0;

Environment::Environment()
    : signal_loss_model_(std::unique_ptr<SignalLossModel>(new LogSignalLossModel())){};

void Environment::Run() {
  while (!events_.empty()) {
    auto event = std::move(events_.front());
    events_.pop_front();
    // Make sure that time is always moving forward
    ZX_ASSERT(event->time >= time_);
    time_ = event->time;
    // Send event to client who requested it
    event->requester->ReceiveNotification(event->payload);
  }
}

void Environment::Tx(const SimFrame* frame, const WlanTxInfo& tx_info, StationIfc* sender) {
  for (auto& staLocPair : stations_) {
    StationIfc* sta = staLocPair.first;
    if (sta != sender) {
      double signal_strength =
          signal_loss_model_->CalcSignalStrength(stations_.at(sender), stations_.at(sta));
      WlanRxInfo rx_info = {.channel = tx_info.channel, .signal_strength = signal_strength};
      // Only deliver frame if the station is sensitive enough to pick it up
      if (signal_strength >= sta->rx_sensitivity_) {
        sta->Rx(frame, rx_info);
      }
    }
  }
}

zx_status_t Environment::ScheduleNotification(StationIfc* sta, zx::duration delay, void* payload,
                                              uint64_t* id_out) {
  uint64_t id = event_count_++;

  // Disallow past events
  if (delay < zx::usec(0)) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto event = std::make_unique<EnvironmentEvent>();
  event->id = id;
  event->time = time_ + delay;
  event->requester = sta;
  event->payload = payload;

  // Keep our events sorted in ascending order of absolute time. When multiple events are
  // scheduled for the same time, the first requested will be processed first.
  auto event_iter = events_.begin();
  while (event_iter != events_.end() && (*event_iter)->time <= event->time) {
    event_iter++;
  }
  events_.insert(event_iter, std::move(event));

  if (id_out != nullptr) {
    *id_out = id;
  }

  return ZX_OK;
}

// Since all events are processed synchronously, we don't have to worry about locking.
zx_status_t Environment::CancelNotification(StationIfc* sta, uint64_t id) {
  for (auto& event_iter : events_) {
    if (event_iter->requester == sta && event_iter->id == id) {
      events_.remove(event_iter);
      return ZX_OK;
    }
  }
  return ZX_ERR_NOT_FOUND;
}

}  // namespace wlan::simulation
