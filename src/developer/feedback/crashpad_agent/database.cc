// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/crashpad_agent/database.h"

#include <memory>

#include "src/developer/feedback/crashpad_agent/report_util.h"
#include "src/lib/files/directory.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/syslog/cpp/logger.h"
#include "third_party/crashpad/client/prune_crash_reports.h"
#include "third_party/crashpad/util/misc/metrics.h"

namespace feedback {

using crashpad::FileReader;
using crashpad::UUID;
using CrashSkippedReason = crashpad::Metrics::CrashSkippedReason;
using OperationStatus = crashpad::CrashReportDatabase::OperationStatus;

constexpr char kCrashpadDatabasePath[] = "/tmp/crashes";

std::unique_ptr<Database> Database::TryCreate(CrashpadDatabaseConfig config) {
  if (!files::IsDirectory(kCrashpadDatabasePath)) {
    files::CreateDirectory(kCrashpadDatabasePath);
  }

  auto crashpad_database =
      crashpad::CrashReportDatabase::Initialize(base::FilePath(kCrashpadDatabasePath));
  if (!crashpad_database) {
    FX_LOGS(ERROR) << fxl::StringPrintf("Error initializing local crash report database at %s",
                                        kCrashpadDatabasePath);
    return nullptr;
  }

  return std::unique_ptr<Database>(new Database(config, std::move(crashpad_database)));
}

Database::Database(CrashpadDatabaseConfig config,
                   std::unique_ptr<crashpad::CrashReportDatabase> database)
    : config_(config), database_(std::move(database)) {
  FXL_DCHECK(database_);
}

bool Database::MakeNewReport(const std::map<std::string, fuchsia::mem::Buffer>& attachments,
                             const std::optional<fuchsia::mem::Buffer>& minidump,
                             const std::map<std::string, std::string>& annotations,
                             crashpad::UUID* local_report_id) {
  // Create local Crashpad report.
  std::unique_ptr<crashpad::CrashReportDatabase::NewReport> report;
  if (const auto status = database_->PrepareNewCrashReport(&report);
      status != OperationStatus::kNoError) {
    FX_LOGS(ERROR) << fxl::StringPrintf("Error creating local Crashpad report (%u)", status);
    return false;
  }

  // Write attachments.
  for (const auto& [filename, content] : attachments) {
    AddAttachment(filename, content, report.get());
  }

  // Optionally write minidump.
  if (minidump.has_value()) {
    if (!WriteVMO(minidump.value(), report->Writer())) {
      FX_LOGS(WARNING) << "Error attaching minidump to Crashpad report";
    }
  }

  // Finish new local Crashpad report.
  if (const auto status = database_->FinishedWritingCrashReport(std::move(report), local_report_id);
      status != OperationStatus::kNoError) {
    FX_LOGS(ERROR) << fxl::StringPrintf("Error writing local Crashpad report (%u)", status);
    return false;
  }

  additional_data_[*local_report_id] = {annotations, minidump.has_value()};
  return true;
}

std::unique_ptr<UploadReport> Database::GetUploadReport(const UUID& local_report_id) {
  if (additional_data_.find(local_report_id) == additional_data_.end()) {
    FX_LOGS(ERROR) << fxl::StringPrintf("Error fetching additional data for local crash report %s",
                                        local_report_id.ToString().c_str());
    // The database no longer contains the report (it was most likely pruned).
    return nullptr;
  }

  auto upload_report = std::make_unique<const crashpad::CrashReportDatabase::UploadReport>();
  if (const auto status = database_->GetReportForUploading(local_report_id, &upload_report);
      status != OperationStatus::kNoError) {
    FX_LOGS(ERROR) << fxl::StringPrintf(
        "Error getting upload report for local id %s from the database (%u)",
        local_report_id.ToString().c_str(), status);
    return nullptr;
  }

  return std::make_unique<UploadReport>(std::move(upload_report),
                                        additional_data_.at(local_report_id).annotations,
                                        additional_data_.at(local_report_id).has_minidump);
}

bool Database::MarkAsUploaded(std::unique_ptr<UploadReport> upload_report,
                              const std::string& server_report_id) {
  if (!upload_report) {
    FX_LOGS(ERROR) << "upload report is null";
    return false;
  }

  const UUID local_report_id = upload_report->GetUUID();
  if (const auto status =
          database_->RecordUploadComplete(upload_report->TransferUploadReport(), server_report_id);
      status != OperationStatus::kNoError) {
    FX_LOGS(ERROR) << fxl::StringPrintf(
        "Unable to record local crash report %s as uploaded in the database (%u)",
        local_report_id.ToString().c_str(), status);
    return false;
  }

  CleanUp(local_report_id);
  return true;
}

bool Database::Archive(const crashpad::UUID& local_report_id) {
  FX_LOGS(INFO) << fxl::StringPrintf("Archiving local crash report, ID %s, under %s",
                                     local_report_id.ToString().c_str(), kCrashpadDatabasePath);

  if (const auto status =
          database_->SkipReportUpload(local_report_id, CrashSkippedReason::kUploadFailed);
      status != OperationStatus::kNoError) {
    FX_LOGS(ERROR) << fxl::StringPrintf(
        "Unable to record local crash report %s as skipped in the database (%u)",
        local_report_id.ToString().c_str(), status);
    return false;
  }

  CleanUp(local_report_id);
  return true;
}

void Database::CleanUp(const UUID& local_report_id) { additional_data_.erase(local_report_id); }

size_t Database::GarbageCollect() {
  // We need to create a new condition every time we prune as it internally maintains a cumulated
  // total size as it iterates over the reports in the database and we want to reset that cumulated
  // total size every time we prune.
  crashpad::DatabaseSizePruneCondition pruning_condition(config_.max_size_in_kb);
  const size_t num_pruned = crashpad::PruneCrashReportDatabase(database_.get(), &pruning_condition);
  if (num_pruned > 0) {
    FX_LOGS(INFO) << fxl::StringPrintf("Pruned %lu crash report(s) from Crashpad database",
                                       num_pruned);
  }

  // We set the |lockfile_ttl| to one day to ensure that reports in new aren't removed until
  // a period of time has passed in which it is certain they are orphaned.
  const size_t num_cleaned = database_->CleanDatabase(/*lockfile_ttl=*/60 * 60 * 24);
  if (num_cleaned > 0) {
    FX_LOGS(INFO) << fxl::StringPrintf("Cleaned %lu crash report(s) from Crashpad database",
                                       num_cleaned);
  }

  if (num_pruned + num_cleaned > 0) {
    // We need to store the |UUID|s to be removed from |additional_data_| because erasing a
    // |UUID| within the loop will invalidate the generated iterator and cause a use-after-free bug.
    std::vector<UUID> clean_up;
    crashpad::CrashReportDatabase::Report report;
    for (const auto& [uuid, _] : additional_data_) {
      if (database_->LookUpCrashReport(uuid, &report) != OperationStatus::kNoError) {
        clean_up.push_back(uuid);
      }
    }

    for (const auto& uuid : clean_up) {
      CleanUp(uuid);
    }
  }

  return num_pruned + num_cleaned;
}

}  // namespace feedback
