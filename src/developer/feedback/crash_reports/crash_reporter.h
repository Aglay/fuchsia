// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_CRASH_REPORTER_H_
#define SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_CRASH_REPORTER_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/async/dispatcher.h>
#include <lib/sys/cpp/service_directory.h>

#include <memory>

#include "src/developer/feedback/crash_reports/config.h"
#include "src/developer/feedback/crash_reports/crash_server.h"
#include "src/developer/feedback/crash_reports/info/crash_reporter_info.h"
#include "src/developer/feedback/crash_reports/info/info_context.h"
#include "src/developer/feedback/crash_reports/privacy_settings_ptr.h"
#include "src/developer/feedback/crash_reports/queue.h"
#include "src/developer/feedback/crash_reports/settings.h"
#include "src/developer/feedback/utils/fidl/data_provider_ptr.h"
#include "src/developer/feedback/utils/fidl/device_id_provider_ptr.h"
#include "src/developer/feedback/utils/utc_time_provider.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/timekeeper/clock.h"

namespace feedback {

class CrashReporter : public fuchsia::feedback::CrashReporter {
 public:
  // Static factory method.
  //
  // Returns nullptr if the crash reporter cannot be instantiated, e.g., because the local report
  // database cannot be accessed.
  static std::unique_ptr<CrashReporter> TryCreate(async_dispatcher_t* dispatcher,
                                                  std::shared_ptr<sys::ServiceDirectory> services,
                                                  const timekeeper::Clock& clock,
                                                  std::shared_ptr<InfoContext> info_context,
                                                  const Config* config);
  static std::unique_ptr<CrashReporter> TryCreate(async_dispatcher_t* dispatcher,
                                                  std::shared_ptr<sys::ServiceDirectory> services,
                                                  const timekeeper::Clock& clock,
                                                  std::shared_ptr<InfoContext> info_context,
                                                  const Config* config,
                                                  std::unique_ptr<CrashServer> crash_server);

  // |fuchsia::feedback::CrashReporter|
  void File(fuchsia::feedback::CrashReport report, FileCallback callback) override;

 private:
  CrashReporter(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
                const timekeeper::Clock& clock, std::shared_ptr<InfoContext> info_context,
                const Config* config, std::unique_ptr<CrashServer> crash_server,
                std::unique_ptr<Queue> queue);

  async_dispatcher_t* dispatcher_;
  async::Executor executor_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  const Config* config_;
  const UTCTimeProvider utc_provider_;
  const std::unique_ptr<CrashServer> crash_server_;
  const std::unique_ptr<Queue> queue_;
  CrashReporterInfo info_;
  Settings settings_;
  PrivacySettingsWatcher privacy_settings_watcher_;
  fidl::DataProviderPtr data_provider_ptr_;
  fidl::DeviceIdProviderPtr device_id_provider_ptr_;
  const std::string build_version_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CrashReporter);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_CRASH_REPORTER_H_
