// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <utility>

#include <fuchsia/crash/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fdio/io.h>
#include <lib/fxl/files/directory.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/files/path.h>
#include <lib/fxl/strings/concatenate.h>
#include <lib/fxl/strings/trim.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/log.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <third_party/crashpad/client/settings.h>
#include <third_party/crashpad/handler/fuchsia/crash_report_exception_handler.h>
#include <third_party/crashpad/handler/minidump_to_upload_parameters.h>
#include <third_party/crashpad/minidump/minidump_file_writer.h>
#include <third_party/crashpad/snapshot/minidump/process_snapshot_minidump.h>
#include <third_party/crashpad/third_party/mini_chromium/mini_chromium/base/files/file_path.h>
#include <third_party/crashpad/third_party/mini_chromium/mini_chromium/base/files/scoped_file.h>
#include <third_party/crashpad/util/file/file_io.h>
#include <third_party/crashpad/util/file/file_reader.h>
#include <third_party/crashpad/util/misc/metrics.h>
#include <third_party/crashpad/util/misc/uuid.h>
#include <third_party/crashpad/util/net/http_body.h>
#include <third_party/crashpad/util/net/http_headers.h>
#include <third_party/crashpad/util/net/http_multipart_builder.h>
#include <third_party/crashpad/util/net/http_transport.h>
#include <third_party/crashpad/util/net/url.h>
#include <zircon/boot/image.h>
#include <zircon/device/sysinfo.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>
#include <zircon/syscalls/object.h>

namespace {

const char kLocalCrashDatabase[] = "/data/crashes";
const char kURL[] = "https://clients2.google.com/cr/report";

class ScopedStoppable {
 public:
  ScopedStoppable() = default;

  ~ScopedStoppable() {
    if (stoppable_) {
      stoppable_->Stop();
    }
  }

  void Reset(crashpad::Stoppable* stoppable) { stoppable_.reset(stoppable); }

  crashpad::Stoppable* Get() { return stoppable_.get(); }

 private:
  std::unique_ptr<crashpad::Stoppable> stoppable_;

  DISALLOW_COPY_AND_ASSIGN(ScopedStoppable);
};

class ScopedUnlink {
 public:
  ScopedUnlink(const std::string& filename) : filename_(filename) {}
  ~ScopedUnlink() { unlink(filename_.c_str()); }

  bool is_valid() const { return !filename_.empty(); }
  const std::string& get() const { return filename_; }

 private:
  std::string filename_;
  DISALLOW_COPY_AND_ASSIGN(ScopedUnlink);
};

std::unique_ptr<crashpad::CrashReportDatabase> GetReportDatabase() {
  if (!files::IsDirectory(kLocalCrashDatabase)) {
    files::CreateDirectory(kLocalCrashDatabase);
  }

  std::unique_ptr<crashpad::CrashReportDatabase> database(
      crashpad::CrashReportDatabase::Initialize(
          base::FilePath(kLocalCrashDatabase)));
  if (!database) {
    FX_LOGS(ERROR) << "error initializing local crash report database at "
                   << kLocalCrashDatabase;
    return nullptr;
  }

  // Today we enable uploads here. In the future, this will most likely be set
  // in some external settings.
  database->GetSettings()->SetUploadsEnabled(true);
  return database;
}

std::string GetSystemLogToFile() {
  std::string filename = files::SimplifyPath(
      fxl::Concatenate({kLocalCrashDatabase, "/log.XXXXXX"}));
  base::ScopedFD fd(mkstemp(filename.data()));
  if (fd.get() < 0) {
    FX_LOGS(ERROR) << "could not create temp file";
    return std::string();
  }

  zx::log log;
  zx_status_t status = zx::log::create(ZX_LOG_FLAG_READABLE, &log);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "zx::log::create failed " << status;
    return std::string();
  }

  char buf[ZX_LOG_RECORD_MAX + 1];
  zx_log_record_t* rec = (zx_log_record_t*)buf;
  while (log.read(ZX_LOG_RECORD_MAX, rec, 0) > 0) {
    if (rec->datalen && (rec->data[rec->datalen - 1] == '\n')) {
      rec->datalen--;
    }
    rec->data[rec->datalen] = 0;

    dprintf(fd.get(), "[%05d.%03d] %05" PRIu64 ".%05" PRIu64 "> %s\n",
            (int)(rec->timestamp / 1000000000ULL),
            (int)((rec->timestamp / 1000000ULL) % 1000ULL), rec->pid, rec->tid,
            rec->data);
  }
  return filename;
}

std::string GetVersion() {
  const char kFilepath[] = "/system/data/build/last-update";
  std::string build_timestamp;
  if (!files::ReadFileToString(kFilepath, &build_timestamp)) {
    FX_LOGS(ERROR) << "Failed to read build timestamp from '" << kFilepath
                   << "'.";
    return "unknown";
  }
  return fxl::TrimString(build_timestamp, "\r\n").ToString();
}

std::string GetPackageName(const zx::process& process) {
  char name[ZX_MAX_NAME_LEN];
  if (process.get_property(ZX_PROP_NAME, name, sizeof(name)) == ZX_OK) {
    return std::string(name);
  }
  return std::string("unknown-package");
}

std::string GetBoardName() {
  const char kSysInfoPath[] = "/dev/misc/sysinfo";
  const int fd = open(kSysInfoPath, O_RDWR);
  if (fd < 0) {
    FX_LOGS(ERROR) << "failed to open " << kSysInfoPath;
    return "unknown";
  }

  char board_name[ZBI_BOARD_NAME_LEN];
  const ssize_t n =
      ioctl_sysinfo_get_board_name(fd, board_name, sizeof(board_name));
  if (n <= 0) {
    FX_LOGS(ERROR) << "failed to get board name";
    return "unknown";
  }
  return std::string(board_name);
}

// Most annotations are shared between userspace and kernel crashes.
// Add additional arguments to this function for values that differ between the
// two, e.g., the package name can be extracted from the crashing process in
// userspace, but it's just "kernel" in kernel space.
std::map<std::string, std::string> GetAnnotations(
    const std::string& package_name) {
  return {
      {"product", "Fuchsia"},
      {"version", GetVersion()},
      // We use ptype to benefit from Chrome's "Process type" handling in
      // the UI.
      {"ptype", package_name},
      {"board_name", GetBoardName()},
  };
}

int UploadReport(
    const std::unique_ptr<crashpad::CrashReportDatabase>& database,
    std::unique_ptr<const crashpad::CrashReportDatabase::UploadReport> report,
    const std::map<std::string, std::string>& annotations) {
  // We have to build the MIME multipart message ourselves as all the public
  // Crashpad helpers are asynchronous and we won't be able to know the upload
  // status nor the server report ID.
  crashpad::HTTPMultipartBuilder http_multipart_builder;
  http_multipart_builder.SetGzipEnabled(true);
  for (const auto& kv : annotations) {
    http_multipart_builder.SetFormData(kv.first, kv.second);
  }
  for (const auto& kv : report->GetAttachments()) {
    http_multipart_builder.SetFileAttachment(kv.first, kv.first, kv.second,
                                             "application/octet-stream");
  }
  http_multipart_builder.SetFileAttachment(
      "upload_file_minidump", report->uuid.ToString() + ".dmp",
      report->Reader(), "application/octet-stream");

  std::unique_ptr<crashpad::HTTPTransport> http_transport(
      crashpad::HTTPTransport::Create());
  crashpad::HTTPHeaders content_headers;
  http_multipart_builder.PopulateContentHeaders(&content_headers);
  for (const auto& content_header : content_headers) {
    http_transport->SetHeader(content_header.first, content_header.second);
  }
  http_transport->SetBodyStream(http_multipart_builder.GetBodyStream());
  http_transport->SetTimeout(60.0);  // 1 minute.
  http_transport->SetURL(kURL);

  std::string server_report_id;
  if (!http_transport->ExecuteSynchronously(&server_report_id)) {
    database->SkipReportUpload(
        report->uuid,
        crashpad::Metrics::CrashSkippedReason::kPrepareForUploadFailed);
    FX_LOGS(ERROR) << "error uploading local crash report, ID "
                   << report->uuid.ToString();
    return EXIT_FAILURE;
  }
  database->RecordUploadComplete(std::move(report), server_report_id);
  FX_LOGS(INFO) << "successfully uploaded crash report at "
                   "https://crash.corp.google.com/"
                << server_report_id;

  return EXIT_SUCCESS;
}

std::unique_ptr<const crashpad::CrashReportDatabase::UploadReport>
GetUploadReport(const std::unique_ptr<crashpad::CrashReportDatabase>& database,
                const crashpad::UUID& local_report_id) {
  std::unique_ptr<const crashpad::CrashReportDatabase::UploadReport> report;
  const crashpad::CrashReportDatabase::OperationStatus database_status =
      database->GetReportForUploading(local_report_id, &report);
  if (database_status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error loading local crash report, ID "
                   << local_report_id.ToString() << " (" << database_status
                   << ")";
    return nullptr;
  }
  return report;
}

int UploadReportForUserspace(
    const std::unique_ptr<crashpad::CrashReportDatabase>& database,
    const crashpad::UUID& local_report_id) {
  // Retrieve local report as an "upload" report.
  std::unique_ptr<const crashpad::CrashReportDatabase::UploadReport> report =
      GetUploadReport(database, local_report_id);
  if (!report) {
    return EXIT_FAILURE;
  }

  // For userspace, we read back the annotations from the minidump instead of
  // passing them as argument like for kernel crashes because the Crashpad
  // handler augmented them with the modules' annotations.
  crashpad::FileReader* reader = report->Reader();
  crashpad::FileOffset start_offset = reader->SeekGet();
  crashpad::ProcessSnapshotMinidump minidump_process_snapshot;
  if (!minidump_process_snapshot.Initialize(reader)) {
    database->SkipReportUpload(
        report->uuid,
        crashpad::Metrics::CrashSkippedReason::kPrepareForUploadFailed);
    FX_LOGS(ERROR) << "error processing minidump for local crash report, ID "
                   << local_report_id.ToString();
    return EXIT_FAILURE;
  }
  const std::map<std::string, std::string> annotations =
      crashpad::BreakpadHTTPFormParametersFromMinidump(
          &minidump_process_snapshot);
  if (!reader->SeekSet(start_offset)) {
    database->SkipReportUpload(
        report->uuid,
        crashpad::Metrics::CrashSkippedReason::kPrepareForUploadFailed);
    FX_LOGS(ERROR) << "error processing minidump for local crash report, ID "
                   << local_report_id.ToString();
    return EXIT_FAILURE;
  }

  return UploadReport(database, std::move(report), annotations);
}

int UploadReportForKernel(
    const std::unique_ptr<crashpad::CrashReportDatabase>& database,
    const crashpad::UUID& local_report_id,
    const std::map<std::string, std::string>& annotations) {
  // Retrieve local report as an "upload" report.
  std::unique_ptr<const crashpad::CrashReportDatabase::UploadReport> report =
      GetUploadReport(database, local_report_id);
  if (!report) {
    return EXIT_FAILURE;
  }
  return UploadReport(database, std::move(report), annotations);
}

}  // namespace

int HandleException(zx::process process, zx::thread thread,
                    zx::port exception_port) {
  const std::string package_name = GetPackageName(process);
  FX_LOGS(INFO) << "generating crash report for exception thrown by "
                << package_name;

  std::unique_ptr<crashpad::CrashReportDatabase> database = GetReportDatabase();
  if (!database) {
    return EXIT_FAILURE;
  }

  // Prepare annotations and attachments.
  const std::map<std::string, std::string> annotations =
      GetAnnotations(package_name);
  std::map<std::string, base::FilePath> attachments;
  ScopedUnlink temp_log_file(GetSystemLogToFile());
  if (temp_log_file.is_valid()) {
    attachments["log"] = base::FilePath(temp_log_file.get());
  }

  // Set minidump and create local crash report.
  //   * The annotations will be stored in the minidump of the report and
  //     augmented with modules' annotations.
  //   * The attachments will be stored in the report.
  // We don't pass an upload_thread so we can do the upload ourselves
  // synchronously.
  crashpad::CrashReportExceptionHandler exception_handler(
      database.get(), /*upload_thread=*/nullptr, &annotations, &attachments,
      /*user_stream_data_sources=*/nullptr);
  crashpad::UUID local_report_id;
  if (!exception_handler.HandleExceptionHandles(
          process, thread, zx::unowned_port(exception_port),
          &local_report_id)) {
    database->SkipReportUpload(
        local_report_id,
        crashpad::Metrics::CrashSkippedReason::kPrepareForUploadFailed);
    FX_LOGS(ERROR) << "error handling exception for local crash report, ID "
                   << local_report_id.ToString();
    return EXIT_FAILURE;
  }

  return UploadReportForUserspace(database, local_report_id);
}

int Process(fuchsia::mem::Buffer crashlog) {
  FX_LOGS(INFO) << "generating crash report for previous kernel panic";

  std::unique_ptr<crashpad::CrashReportDatabase> database = GetReportDatabase();
  if (!database) {
    return EXIT_FAILURE;
  }

  crashpad::CrashReportDatabase::OperationStatus database_status;

  // Create report.
  std::unique_ptr<crashpad::CrashReportDatabase::NewReport> report;
  database_status = database->PrepareNewCrashReport(&report);
  if (database_status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error creating local crash report (" << database_status
                   << ")";
    return EXIT_FAILURE;
  }

  // Prepare annotations and attachments.
  const std::map<std::string, std::string> annotations =
      GetAnnotations(/*package_name=*/"kernel");
  crashpad::FileWriter* writer = report->AddAttachment("log");
  if (!writer) {
    return EXIT_FAILURE;
  }
  // TODO(frousseau): make crashpad::FileWriter VMO-aware.
  std::unique_ptr<void, decltype(&free)> buffer(malloc(crashlog.size), &free);
  zx_status_t status = crashlog.vmo.read(buffer.get(), 0u, crashlog.size);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "error writing VMO crashlog to buffer: "
                   << zx_status_get_string(status);
    return EXIT_FAILURE;
  }
  writer->Write(buffer.get(), crashlog.size);

  // Finish new report.
  crashpad::UUID local_report_id;
  database_status =
      database->FinishedWritingCrashReport(std::move(report), &local_report_id);
  if (database_status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error writing local crash report (" << database_status
                   << ")";
    return EXIT_FAILURE;
  }

  return UploadReportForKernel(database, local_report_id, annotations);
}

class AnalyzerImpl : public fuchsia::crash::Analyzer {
 public:
  // fuchsia::crash::Analyzer:
  void Analyze(::zx::process process, ::zx::thread thread,
               ::zx::port exception_port, AnalyzeCallback callback) override {
    callback();
    if (HandleException(std::move(process), std::move(thread),
                        std::move(exception_port)) != EXIT_SUCCESS) {
      FX_LOGS(ERROR) << "failed to handle exception. Won't retry.";
    }
  }

  void Process(fuchsia::mem::Buffer crashlog,
               ProcessCallback callback) override {
    callback();
    if (::Process(fbl::move(crashlog)) != EXIT_SUCCESS) {
      FX_LOGS(ERROR) << "failed to process VMO crashlog. Won't retry.";
    }
  }
};

int main(int argc, const char** argv) {
  syslog::InitLogger({"crash"});

  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  std::unique_ptr<component::StartupContext> app_context(
      component::StartupContext::CreateFromStartupInfo());

  AnalyzerImpl analyzer;

  fidl::BindingSet<fuchsia::crash::Analyzer> bindings;

  app_context->outgoing().AddPublicService(bindings.GetHandler(&analyzer));

  loop.Run();

  return 0;
}
