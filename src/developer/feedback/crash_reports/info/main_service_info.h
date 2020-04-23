// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_INFO_MAIN_SERVICE_INFO_H_
#define SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_INFO_MAIN_SERVICE_INFO_H_

#include <memory>

#include "src/developer/feedback/crash_reports/config.h"
#include "src/developer/feedback/crash_reports/info/info_context.h"
#include "src/developer/feedback/utils/inspect_protocol_stats.h"

namespace feedback {

// Information about the agent we want to export.
struct MainServiceInfo {
 public:
  MainServiceInfo(std::shared_ptr<InfoContext> context);

  // Exposes the static configuration of the agent.
  void ExposeConfig(const feedback::Config& config);

  // Updates stats related to fuchsia.feedback.CrashReporter.
  void UpdateCrashReporterProtocolStats(InspectProtocolStatsUpdateFn update);

 private:
  std::shared_ptr<InfoContext> context_;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_INFO_MAIN_SERVICE_INFO_H_
