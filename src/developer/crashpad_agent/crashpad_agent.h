// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_CRASHPAD_AGENT_CRASHPAD_AGENT_H_
#define SRC_DEVELOPER_CRASHPAD_AGENT_CRASHPAD_AGENT_H_

#include <string>
#include <utility>

#include <fuchsia/crash/cpp/fidl.h>
#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fidl/cpp/string.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/port.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <zircon/status.h>

#include "src/developer/crashpad_agent/config.h"
#include "src/developer/crashpad_agent/crash_server.h"
#include "src/lib/fxl/macros.h"
#include "third_party/crashpad/client/crash_report_database.h"
#include "third_party/crashpad/util/misc/uuid.h"

namespace fuchsia {
namespace crash {

class CrashpadAgent : public Analyzer {
 public:
  // Static factory methods.
  // Returns nullptr if the agent cannot be instantiated, e.g., because the
  // local report database cannot be accessed.
  static std::unique_ptr<CrashpadAgent> TryCreate(
      std::shared_ptr<::sys::ServiceDirectory> services);
  static std::unique_ptr<CrashpadAgent> TryCreate(
      std::shared_ptr<::sys::ServiceDirectory> services, Config config);
  static std::unique_ptr<CrashpadAgent> TryCreate(
      std::shared_ptr<::sys::ServiceDirectory> services, Config config,
      std::unique_ptr<CrashServer> crash_server);

  void OnNativeException(zx::process process, zx::thread thread,
                         zx::port exception_port,
                         OnNativeExceptionCallback callback) override;

  void OnManagedRuntimeException(
      std::string component_url, ManagedRuntimeException exception,
      OnManagedRuntimeExceptionCallback callback) override;

  void OnKernelPanicCrashLog(fuchsia::mem::Buffer crash_log,
                             OnKernelPanicCrashLogCallback callback) override;

 private:
  CrashpadAgent(std::shared_ptr<::sys::ServiceDirectory> services,
                Config config,
                std::unique_ptr<crashpad::CrashReportDatabase> database,
                std::unique_ptr<CrashServer> crash_server);

  zx_status_t OnNativeException(zx::process process, zx::thread thread,
                                zx::port exception_port);
  zx_status_t OnManagedRuntimeException(std::string component_url,
                                        ManagedRuntimeException exception);
  zx_status_t OnKernelPanicCrashLog(fuchsia::mem::Buffer crash_log);

  zx_status_t GetFeedbackData(fuchsia::feedback::Data* data);
  fuchsia::feedback::Data GetFeedbackData();

  // Uploads local crash report of ID |local_report_id|, attaching either the
  // passed |annotations| or reading the annotations from its minidump.
  //
  // Either |annotations| or |read_annotations_from_minidump| must be set, but
  // only one of them.
  zx_status_t UploadReport(
      const crashpad::UUID& local_report_id,
      const std::map<std::string, std::string>* annotations,
      bool read_annotations_from_minidump);

  // Deletes oldest crash reports to keep |database_| under a maximum size read
  // from |config_|.
  //
  // Report age is defined by their
  // crashpad::CrashReportDatabase::Report::creation_time.
  void PruneDatabase();

  const std::shared_ptr<::sys::ServiceDirectory> services_;
  const Config config_;
  const std::unique_ptr<crashpad::CrashReportDatabase> database_;
  const std::unique_ptr<CrashServer> crash_server_;

  fuchsia::feedback::DataProviderSyncPtr feedback_data_provider_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CrashpadAgent);
};

}  // namespace crash
}  // namespace fuchsia

#endif  // SRC_DEVELOPER_CRASHPAD_AGENT_CRASHPAD_AGENT_H_
