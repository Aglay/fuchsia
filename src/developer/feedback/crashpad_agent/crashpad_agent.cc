// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/crashpad_agent/crashpad_agent.h"

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fit/result.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "src/developer/feedback/crashpad_agent/config.h"
#include "src/developer/feedback/crashpad_agent/crash_server.h"
#include "src/developer/feedback/crashpad_agent/feedback_data_provider_ptr.h"
#include "src/developer/feedback/crashpad_agent/report_util.h"
#include "src/developer/feedback/utils/cobalt_metrics.h"
#include "src/lib/files/file.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/syslog/cpp/logger.h"

namespace feedback {
namespace {

using fuchsia::feedback::CrashReport;
using fuchsia::feedback::Data;

const char kDefaultConfigPath[] = "/pkg/data/default_config.json";
const char kOverrideConfigPath[] = "/config/data/override_config.json";

// This should be kept higher than the timeout the component serving fuchsia.feedback.DataProvider
// has on its side for each feedback data as we pay the price for making the request (establishing
// the connection, potentially spawning the serving component for the first time, getting the
// response, etc.) .
constexpr zx::duration kFeedbackDataCollectionTimeout = zx::sec(30) + /*some slack*/ zx::sec(5);

}  // namespace

std::unique_ptr<CrashpadAgent> CrashpadAgent::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
    std::shared_ptr<InfoContext> info_context) {
  Config config;

  // We use the default config included in the package of this component if no override config was
  // specified or if we failed to parse the override config.
  bool use_default_config = true;

  if (files::IsFile(kOverrideConfigPath)) {
    use_default_config = false;
    if (const zx_status_t status = ParseConfig(kOverrideConfigPath, &config); status != ZX_OK) {
      // We failed to parse the override config: fall back to the default config.
      use_default_config = true;
      FX_PLOGS(ERROR, status) << "Failed to read override config file at " << kOverrideConfigPath
                              << " - falling back to default config file";
    }
  }

  // Either there was no override config or we failed to parse it.
  if (use_default_config) {
    if (const zx_status_t status = ParseConfig(kDefaultConfigPath, &config); status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to read default config file at " << kDefaultConfigPath;

      FX_LOGS(FATAL) << "Failed to set up crash analyzer";
      return nullptr;
    }
  }

  return CrashpadAgent::TryCreate(dispatcher, std::move(services), std::move(info_context),
                                  std::move(config));
}

std::unique_ptr<CrashpadAgent> CrashpadAgent::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
    std::shared_ptr<InfoContext> info_context, Config config) {
  std::unique_ptr<CrashServer> crash_server;
  if (config.crash_server.url) {
    crash_server = std::make_unique<CrashServer>(*config.crash_server.url);
  }
  return CrashpadAgent::TryCreate(dispatcher, std::move(services), std::move(info_context),
                                  std::move(config), std::move(crash_server));
}

std::unique_ptr<CrashpadAgent> CrashpadAgent::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
    std::shared_ptr<InfoContext> info_context, Config config,
    std::unique_ptr<CrashServer> crash_server) {
  auto queue = Queue::TryCreate(dispatcher, info_context, crash_server.get());
  if (!queue) {
    FX_LOGS(FATAL) << "Failed to set up crash reporter";
    return nullptr;
  }
  return std::unique_ptr<CrashpadAgent>(
      new CrashpadAgent(dispatcher, std::move(services), std::move(info_context), std::move(config),
                        std::move(crash_server), std::move(queue)));
}

CrashpadAgent::CrashpadAgent(async_dispatcher_t* dispatcher,
                             std::shared_ptr<sys::ServiceDirectory> services,
                             std::shared_ptr<InfoContext> info_context, Config config,
                             std::unique_ptr<CrashServer> crash_server,
                             std::unique_ptr<Queue> queue)
    : dispatcher_(dispatcher),
      executor_(dispatcher),
      services_(services),
      config_(std::move(config)),
      queue_(std::move(queue)),
      crash_server_(std::move(crash_server)),
      info_(std::move(info_context)),
      privacy_settings_watcher_(services_, &settings_) {
  FXL_DCHECK(dispatcher_);
  FXL_DCHECK(services_);
  FXL_DCHECK(queue_);
  if (config.crash_server.url) {
    FXL_DCHECK(crash_server_);
  }

  const auto& upload_policy = config_.crash_server.upload_policy;
  settings_.set_upload_policy(upload_policy);
  if (upload_policy == CrashServerConfig::UploadPolicy::READ_FROM_PRIVACY_SETTINGS) {
    privacy_settings_watcher_.StartWatching();
  }

  queue_->WatchSettings(&settings_);

  info_.ExposeConfig(config_);
  info_.ExposeSettings(&settings_);
}

void CrashpadAgent::File(fuchsia::feedback::CrashReport report, FileCallback callback) {
  if (!report.has_program_name()) {
    FX_LOGS(ERROR) << "Invalid crash report. No program name. Won't file.";
    callback(fit::error(ZX_ERR_INVALID_ARGS));
    info_.LogCrashState(CrashState::kDropped);
    return;
  }
  FX_LOGS(INFO) << "Generating crash report for " << report.program_name();

  auto promise = GetFeedbackData(dispatcher_, services_, kFeedbackDataCollectionTimeout)
                     .then([this, report = std::move(report)](
                               fit::result<Data>& result) mutable -> fit::result<void> {
                       Data feedback_data;
                       if (result.is_ok()) {
                         feedback_data = result.take_value();
                       }

                       const std::string program_name = report.program_name();

                       std::map<std::string, std::string> annotations;
                       std::map<std::string, fuchsia::mem::Buffer> attachments;
                       std::optional<fuchsia::mem::Buffer> minidump;
                       BuildAnnotationsAndAttachments(std::move(report), std::move(feedback_data),
                                                      &annotations, &attachments, &minidump);

                       if (!queue_->Add(program_name, std::move(attachments), std::move(minidump),
                                        annotations)) {
                         FX_LOGS(ERROR) << "Error adding new report to the queue";
                         info_.LogCrashState(CrashState::kDropped);
                         return fit::error();
                       }

                       info_.LogCrashState(CrashState::kFiled);
                       return fit::ok();
                     })
                     .then([callback = std::move(callback)](fit::result<void>& result) {
                       if (result.is_error()) {
                         FX_LOGS(ERROR) << "Failed to file crash report. Won't retry.";
                         callback(fit::error(ZX_ERR_INTERNAL));
                       } else {
                         callback(fit::ok());
                       }
                     });

  executor_.schedule_task(std::move(promise));
}

}  // namespace feedback
