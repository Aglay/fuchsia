// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_INFO_CRASH_REPORTER_INFO_H_
#define SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_INFO_CRASH_REPORTER_INFO_H_

#include <memory>

#include "src/developer/feedback/crash_reports/info/info_context.h"
#include "src/developer/feedback/crash_reports/settings.h"
#include "src/developer/feedback/utils/cobalt/metrics.h"

namespace feedback {

// Information about the crash reporter we want to export.
struct CrashReporterInfo {
 public:
  CrashReporterInfo(std::shared_ptr<InfoContext> context);

  // Exposes the mutable settings of the crash reporter.
  void ExposeSettings(feedback::Settings* settings);

  void LogCrashState(cobalt::CrashState state);

 private:
  std::shared_ptr<InfoContext> context_;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_CRASH_REPORTS_INFO_CRASH_REPORTER_INFO_H_
