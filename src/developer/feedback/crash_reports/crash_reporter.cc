// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/crash_reports/crash_reporter.h"

#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fit/promise.h>
#include <lib/fit/result.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "src/developer/feedback/crash_reports/config.h"
#include "src/developer/feedback/crash_reports/crash_server.h"
#include "src/developer/feedback/crash_reports/report_util.h"
#include "src/developer/feedback/utils/cobalt_metrics.h"
#include "src/developer/feedback/utils/fidl/channel_provider_ptr.h"
#include "src/developer/feedback/utils/fit/timeout.h"
#include "src/lib/files/file.h"
#include "src/lib/fxl/strings/trim.h"
#include "src/lib/syslog/cpp/logger.h"

namespace feedback {
namespace {

using fuchsia::feedback::CrashReport;
using fuchsia::feedback::Data;

// Most of the time spent generating a crash report is spent collecting annotations and attachments
// from other services. The timeout should be kept higher than how long any of these services might
// take as we pay the extra price on top of that timeout for making the request (establishing the
// connection, potentially spawning the serving component for the first time, getting the response,
// etc.).
constexpr zx::duration kCrashReportGenerationTimeout =
    zx::sec(30) /*fuchsia.feedback.DataProvider*/ + zx::sec(5) /*some slack*/;

}  // namespace

std::unique_ptr<CrashReporter> CrashReporter::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
    const timekeeper::Clock& clock, std::shared_ptr<InfoContext> info_context,
    const Config* config) {
  std::unique_ptr<CrashServer> crash_server;
  if (config->crash_server.url) {
    crash_server = std::make_unique<CrashServer>(*(config->crash_server.url));
  }

  return TryCreate(dispatcher, std::move(services), clock, std::move(info_context), config,
                   std::move(crash_server));
}

std::unique_ptr<CrashReporter> CrashReporter::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
    const timekeeper::Clock& clock, std::shared_ptr<InfoContext> info_context, const Config* config,
    std::unique_ptr<CrashServer> crash_server) {
  auto queue = Queue::TryCreate(dispatcher, services, info_context, crash_server.get());
  if (!queue) {
    FX_LOGS(FATAL) << "Failed to set up crash reporter";
    return nullptr;
  }

  return std::unique_ptr<CrashReporter>(
      new CrashReporter(dispatcher, std::move(services), clock, std::move(info_context),
                        std::move(config), std::move(crash_server), std::move(queue)));
}

namespace {

std::string ReadStringFromFile(const std::string& filepath) {
  std::string content;
  if (!files::ReadFileToString(filepath, &content)) {
    FX_LOGS(ERROR) << "Failed to read content from " << filepath;
    return "<unknown>";
  }
  return fxl::TrimString(content, "\r\n").ToString();
}

}  // namespace

CrashReporter::CrashReporter(async_dispatcher_t* dispatcher,
                             std::shared_ptr<sys::ServiceDirectory> services,
                             const timekeeper::Clock& clock,
                             std::shared_ptr<InfoContext> info_context, const Config* config,
                             std::unique_ptr<CrashServer> crash_server,
                             std::unique_ptr<Queue> queue)
    : dispatcher_(dispatcher),
      executor_(dispatcher),
      services_(services),
      config_(std::move(config)),
      utc_provider_(services_, clock),
      crash_server_(std::move(crash_server)),
      queue_(std::move(queue)),
      info_(std::move(info_context)),
      privacy_settings_watcher_(dispatcher, services_, &settings_),
      data_provider_ptr_(dispatcher_, services_),
      device_id_provider_ptr_(dispatcher_, services_),
      build_version_(ReadStringFromFile("/config/build-info/version")) {
  FX_CHECK(dispatcher_);
  FX_CHECK(services_);
  if (config->crash_server.url) {
    FX_CHECK(crash_server_);
  }
  FX_CHECK(queue_);

  const auto& upload_policy = config_->crash_server.upload_policy;
  settings_.set_upload_policy(upload_policy);
  if (upload_policy == CrashServerConfig::UploadPolicy::READ_FROM_PRIVACY_SETTINGS) {
    privacy_settings_watcher_.StartWatching();
  }

  queue_->WatchSettings(&settings_);

  info_.ExposeSettings(&settings_);
}

void CrashReporter::File(fuchsia::feedback::CrashReport report, FileCallback callback) {
  if (!report.has_program_name()) {
    FX_LOGS(ERROR) << "Invalid crash report. No program name. Won't file.";
    callback(::fit::error(ZX_ERR_INVALID_ARGS));
    info_.LogCrashState(CrashState::kDropped);
    return;
  }
  FX_LOGS(INFO) << "Generating crash report for " << report.program_name();

  auto channel_promise =
      fidl::GetCurrentChannel(dispatcher_, services_, fit::Timeout(kCrashReportGenerationTimeout));
  auto data_promise = data_provider_ptr_.GetData(kCrashReportGenerationTimeout);
  auto device_id_promise = device_id_provider_ptr_.GetId(kCrashReportGenerationTimeout);

  auto promise =
      ::fit::join_promises(std::move(channel_promise), std::move(data_promise),
                           std::move(device_id_promise))
          .then([this, report = std::move(report)](
                    ::fit::result<std::tuple<::fit::result<std::string>, ::fit::result<Data>,
                                             ::fit::result<std::string>>>& results) mutable
                -> ::fit::result<void> {
            if (results.is_error()) {
              return ::fit::error();
            }

            auto channel_result = std::move(std::get<0>(results.value()));
            std::optional<std::string> channel = std::nullopt;
            if (channel_result.is_ok()) {
              channel = channel_result.take_value();
            }

            auto data_result = std::move(std::get<1>(results.value()));
            Data feedback_data;
            if (data_result.is_ok()) {
              feedback_data = data_result.take_value();
            }

            auto device_id_result = std::move(std::get<2>(results.value()));
            std::optional<std::string> device_id = std::nullopt;
            if (device_id_result.is_ok()) {
              device_id = device_id_result.take_value();
            }

            const std::string program_name = report.program_name();

            std::map<std::string, std::string> annotations;
            std::map<std::string, fuchsia::mem::Buffer> attachments;
            std::optional<fuchsia::mem::Buffer> minidump;
            BuildAnnotationsAndAttachments(std::move(report), std::move(feedback_data),
                                           utc_provider_.CurrentTime(), device_id, build_version_,
                                           channel, &annotations, &attachments, &minidump);

            if (!queue_->Add(program_name, std::move(attachments), std::move(minidump),
                             annotations)) {
              FX_LOGS(ERROR) << "Error adding new report to the queue";
              info_.LogCrashState(CrashState::kDropped);
              return ::fit::error();
            }

            info_.LogCrashState(CrashState::kFiled);
            return ::fit::ok();
          })
          .then([callback = std::move(callback)](::fit::result<void>& result) {
            if (result.is_error()) {
              FX_LOGS(ERROR) << "Failed to file crash report. Won't retry.";
              callback(::fit::error(ZX_ERR_INTERNAL));
            } else {
              callback(::fit::ok());
            }
          });

  executor_.schedule_task(std::move(promise));
}

}  // namespace feedback
