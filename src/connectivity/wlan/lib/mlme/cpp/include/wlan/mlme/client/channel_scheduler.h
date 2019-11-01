// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CHANNEL_SCHEDULER_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CHANNEL_SCHEDULER_H_

#include <memory>

#include <ddk/protocol/wlan/info.h>
#include <wlan/mlme/client/timeout_target.h>
#include <wlan/mlme/device_interface.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/timer.h>
#include <wlan/mlme/timer_manager.h>

namespace wlan {

struct OffChannelHandler;

struct OffChannelRequest {
  wlan_channel_t chan;
  zx::duration duration;
  OffChannelHandler* handler;
};

struct OffChannelHandler {
  virtual void BeginOffChannelTime() = 0;
  virtual void HandleOffChannelFrame(std::unique_ptr<Packet>) = 0;

  // Invoked to end current off channel time and switch to another channel.
  // (a) If switching to an off-channel, fill |next_req| and return `true` to
  // schedule another
  //     off-channel request.
  // (b) If switching back on channel, return `false`
  virtual bool EndOffChannelTime(bool interrupted, OffChannelRequest* next_req) = 0;

  virtual ~OffChannelHandler() = default;
};

struct OnChannelHandler {
  virtual void HandleOnChannelFrame(std::unique_ptr<Packet>) = 0;
  virtual void PreSwitchOffChannel() = 0;
  virtual void ReturnedOnChannel() = 0;
};

class ChannelScheduler {
 public:
  ChannelScheduler(OnChannelHandler* handler, DeviceInterface* device,
                   TimerManager<TimeoutTarget>* timer_mgr);

  void HandleIncomingFrame(std::unique_ptr<Packet>);

  // Set the 'on' channel. If we are currently on the main channel,
  // switch to the new main channel.
  zx_status_t SetChannel(const wlan_channel_t& chan);

  // Return true if we are currently on the main channel
  bool OnChannel() const { return on_channel_; }

  // Switch on channel immediately and ensure that we stay there
  // at least until |end|
  void EnsureOnChannel(zx::time end);

  // Request an off-channel time. Any previously existing request will be
  // dropped. Off-channel time might not begin immediately.
  // |request.handler->BeginOffChannelTime| will be called when the off-channel
  // time begins.
  void RequestOffChannelTime(const OffChannelRequest& request);

  void ScheduleTimeout(zx::time deadline);
  void HandleTimeout();
  void CancelTimeout();

 private:
  void GoOffChannel();
  void ResetTimer(zx::time deadline);

  OnChannelHandler* on_channel_handler_;
  DeviceInterface* device_;
  TimerManager<TimeoutTarget>* timer_mgr_;

  wlan_channel_t channel_ = {.primary = 1, .cbw = WLAN_CHANNEL_BANDWIDTH__20, .secondary80 = 0};
  bool on_channel_ = true;
  bool ensure_on_channel_ = false;
  bool pending_off_channel_request_ = false;
  OffChannelRequest off_channel_request_;
  TimeoutId timeout_;
};

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CHANNEL_SCHEDULER_H_
