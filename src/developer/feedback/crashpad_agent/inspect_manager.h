// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_INSPECT_MANAGER_H_
#define SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_INSPECT_MANAGER_H_

#include <lib/inspect/cpp/vmo/types.h>
#include <lib/timekeeper/clock.h>

#include <map>
#include <string>
#include <vector>

#include "src/developer/feedback/crashpad_agent/config.h"
#include "src/developer/feedback/crashpad_agent/settings.h"
#include "src/lib/fxl/macros.h"

namespace feedback {

// Encapsulates the global state exposed through Inspect.
class InspectManager {
 public:
  InspectManager(inspect::Node* root_node, timekeeper::Clock* clock);

  // Exposes the static configuration of the crash reporter.
  void ExposeConfig(const feedback::Config& config);

  // Exposes the mutable settings of the crash reporter.
  void ExposeSettings(feedback::Settings* settings);

  // Adds a new report under the given program.
  //
  // Returns false if there is already a report with |local_report_id| as ID (for the given program
  // or another).
  bool AddReport(const std::string& program_name, const std::string& local_report_id);

  // Marks an existing report as uploaded, storing its server report ID.
  //
  // Returns false if there are no reports with |local_report_id| as ID.
  bool MarkReportAsUploaded(const std::string& local_report_id,
                            const std::string& server_report_id);

 private:
  // Callback to update |settings_| on upload policy changes.
  void OnUploadPolicyChange(const feedback::Settings::UploadPolicy& upload_policy);

  // Returns a non-localized human-readable timestamp of the current time according to |clock_|.
  std::string CurrentTime();

  // Inspect node containing the static configuration.
  struct Config {
    // Inspect node containing the database configuration.
    struct CrashpadDatabaseConfig {
      inspect::Node node;
      inspect::StringProperty path;
      inspect::UintProperty max_size_in_kb;
    };

    // Inspect node containing the crash server configuration.
    struct CrashServerConfig {
      inspect::Node node;
      inspect::StringProperty upload_policy;
      inspect::StringProperty url;
    };

    inspect::Node node;

    CrashpadDatabaseConfig crashpad_database;
    CrashServerConfig crash_server;
  };

  // Inspect node containing the mutable settings.
  struct Settings {
    inspect::Node node;

    inspect::StringProperty upload_policy;
  };

  // Inspect node for a single report.
  struct Report {
    Report(inspect::Node* parent_node, const std::string& local_report_id,
           const std::string& creation_time);
    Report(Report&&) = default;

    // Adds the crash server entries after receiving a server response.
    void MarkAsUploaded(const std::string& server_report_id, const std::string& creation_time);

   private:
    inspect::Node node_;
    inspect::StringProperty creation_time_;

    inspect::Node server_node_;
    inspect::StringProperty server_id_;
    inspect::StringProperty server_creation_time_;

    FXL_DISALLOW_COPY_AND_ASSIGN(Report);
  };

  // Inspect node containing a list of reports for a given program.
  struct ReportList {
    inspect::Node node;
    // Allocate all reports on the heap so we can use pointers to them in |Reports|.
    std::vector<std::unique_ptr<Report>> reports;
  };

  // Inspect node pointing to the list of reports.
  struct Reports {
    inspect::Node node;
    // Maps a program name to a list of |Report| nodes.
    std::map<std::string, ReportList> program_name_to_report_lists;
    // Maps a local report ID to the corresponding |Report|
    std::map<std::string, Report*> local_report_id_to_report;
  };

  inspect::Node* root_node_ = nullptr;
  timekeeper::Clock* clock_;
  Config config_;
  Settings settings_;
  Reports reports_;

  FXL_DISALLOW_COPY_AND_ASSIGN(InspectManager);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_INSPECT_MANAGER_H_
