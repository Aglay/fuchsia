// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback_agent/tests/stub_log_listener.h"

#include <fuchsia/logger/cpp/fidl.h>
#include <lib/zx/time.h>
#include <stdint.h>

#include "src/lib/fxl/logging.h"

namespace fuchsia {
namespace feedback {
namespace {

constexpr zx_time_t kLogMessageBaseTimestamp = ZX_SEC(15604);
constexpr uint64_t kLogMessageProcessId = 7559;
constexpr uint64_t kLogMessageThreadId = 7687;

}  // namespace

fuchsia::logger::LogMessage BuildLogMessage(
    const int32_t severity, const std::string& text,
    const zx_time_t timestamp_offset, const std::vector<std::string>& tags) {
  fuchsia::logger::LogMessage msg{};
  msg.time = kLogMessageBaseTimestamp + timestamp_offset;
  msg.pid = kLogMessageProcessId;
  msg.tid = kLogMessageThreadId;
  msg.tags = tags;
  msg.severity = severity;
  msg.msg = text;
  return msg;
}

void StubLogger::DumpLogs(
    fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
    std::unique_ptr<fuchsia::logger::LogFilterOptions> options) {
  fuchsia::logger::LogListenerPtr log_listener_ptr = log_listener.Bind();
  FXL_CHECK(log_listener_ptr.is_bound());
  log_listener_ptr->LogMany(messages_);
  log_listener_ptr->Done();
}

void StubLoggerNeverBindsToLogListener::DumpLogs(
    fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
    std::unique_ptr<fuchsia::logger::LogFilterOptions> options) {}

void StubLoggerUnbindsAfterOneMessage::DumpLogs(
    fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
    std::unique_ptr<fuchsia::logger::LogFilterOptions> options) {
  FXL_CHECK(messages_.size() > 1u)
      << "You need to set up more than one message using set_messages()";
  fuchsia::logger::LogListenerPtr log_listener_ptr = log_listener.Bind();
  FXL_CHECK(log_listener_ptr.is_bound());
  log_listener_ptr->LogMany(std::vector<fuchsia::logger::LogMessage>(
      messages_.begin(), messages_.begin() + 1));
  log_listener_ptr.Unbind();
}

}  // namespace feedback
}  // namespace fuchsia
