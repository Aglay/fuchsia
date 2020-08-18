// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_TIME_NETWORK_TIME_SERVICE_SERVICE_H_
#define SRC_SYS_TIME_NETWORK_TIME_SERVICE_SERVICE_H_

#include <fuchsia/deprecatedtimezone/cpp/fidl.h>
#include <fuchsia/time/external/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/sys/cpp/component_context.h>

#include <vector>

#include "lib/fidl/cpp/binding_set.h"
#include "src/sys/time/lib/network_time/system_time_updater.h"
#include "src/sys/time/lib/network_time/time_server_config.h"
#include "src/sys/time/network_time_service/watcher.h"

const uint64_t kNanosBetweenFailures = 1 * 1'000'000'000u;
const uint64_t kNanosBetweenSuccesses = 30 * 60 * 1'000'000'000u;

namespace time_external = fuchsia::time::external;
namespace network_time_service {

// Defines how the |TimeServiceImpl| PushSource polls for updates.
class RetryConfig {
 public:
  RetryConfig(uint64_t nanos_between_failures = kNanosBetweenFailures,
              uint64_t nanos_between_successes = kNanosBetweenSuccesses)
      : nanos_between_failures(nanos_between_failures),
        nanos_between_successes(nanos_between_successes){};

  uint64_t nanos_between_failures;
  uint64_t nanos_between_successes;
};

// Implementation of the FIDL time services.
// TODO(58068): This currently assumes that there is only a single client. To support
// multiple clients, this needs to retain per-client state so that it understands when
// a value hasn't been returned yet to a particular client, and so that it can close
// channels to only a single client as needed.
class TimeServiceImpl : public fuchsia::deprecatedtimezone::TimeService,
                        public time_external::PushSource {
  // The type of the callback is identical between the two namespaces.
  using fuchsia::deprecatedtimezone::TimeService::UpdateCallback;

 public:
  // Constructs the time service with a caller-owned application context.
  TimeServiceImpl(std::unique_ptr<sys::ComponentContext> context,
                  time_server::SystemTimeUpdater time_updater,
                  time_server::RoughTimeServer rough_time_server, async_dispatcher_t* dispatcher,
                  RetryConfig retry_config = RetryConfig());
  ~TimeServiceImpl();

  // |TimeService|:
  void Update(uint8_t num_retries, UpdateCallback callback) override;

  // |PushSource|:
  void UpdateDeviceProperties(time_external::Properties properties) override;

  // |PushSource|:
  void WatchSample(WatchSampleCallback callback) override;

  // |PushSource|:
  void WatchStatus(WatchStatusCallback callback) override;

 private:
  // Attempt to retrieve UTC and update system time without retries.
  std::optional<zx::time_utc> UpdateSystemTime();

  // Polls for new time samples and post changes to the time source status.
  void AsyncPollSamples(async_dispatcher_t* dispatcher, async::TaskBase* task, zx_status_t status);

  // Schedules a sample poll to begin at the specified time in the dispatcher's clock.
  void ScheduleAsyncPoll(zx::time dispatch_time);

  // Remove the PushSource client with the specified epitaph and reset client state.
  void ResetPushSourceClient(zx_status_t epitaph);

  std::unique_ptr<sys::ComponentContext> context_;
  fidl::BindingSet<fuchsia::deprecatedtimezone::TimeService> deprecated_bindings_;
  time_server::SystemTimeUpdater time_updater_;
  time_server::RoughTimeServer rough_time_server_;

  fidl::Binding<time_external::PushSource> push_source_binding_;
  SampleWatcher sample_watcher_;

  async_dispatcher_t* dispatcher_;
  // Time of last successful update. Reported in the dispatcher's clock which may not be monotonic.
  std::optional<zx::time> dispatcher_last_success_time_;
  async::TaskMethod<TimeServiceImpl, &TimeServiceImpl::AsyncPollSamples> sample_poll_task_{this};
  RetryConfig retry_config_;
};

}  // namespace network_time_service

#endif  // SRC_SYS_TIME_NETWORK_TIME_SERVICE_SERVICE_H_
