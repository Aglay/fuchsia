// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_CHANNEL_PTR_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_CHANNEL_PTR_H_

#include <fuchsia/update/channel/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/bridge.h>
#include <lib/fit/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <zircon/time.h>

#include <memory>
#include <string>
#include <vector>

#include "src/lib/fxl/functional/cancelable_callback.h"
#include "src/lib/fxl/macros.h"

namespace feedback {

// Retrieves the current OTA channel.
//
// fuchsia.update.channel.Provider is expected to be in |services|.
fit::promise<std::string> RetrieveCurrentChannel(async_dispatcher_t* dispatcher,
                                                 std::shared_ptr<sys::ServiceDirectory> services,
                                                 zx::duration timeout);

// Wraps around fuchsia::update::channel::ProviderPtr to handle establishing the connection, losing
// the connection, waiting for the callback, enforcing a timeout, etc.
//
// GetCurrent() is expected to be called only once.
class ChannelProvider {
 public:
  ChannelProvider(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services);

  fit::promise<std::string> GetCurrent(zx::duration timeout);

 private:
  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  // Enforces the one-shot nature of GetChannel().
  bool has_called_get_current_ = false;

  fuchsia::update::channel::ProviderPtr update_info_;
  fit::bridge<std::string> done_;
  // We wrap the delayed task we post on the async loop to timeout in a CancelableClosure so we can
  // cancel it if we are done another way.
  fxl::CancelableClosure done_after_timeout_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ChannelProvider);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_CHANNEL_PTR_H_
