// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_COBALT_APP_LOGGER_IMPL_H_
#define GARNET_BIN_COBALT_APP_LOGGER_IMPL_H_

#include <fuchsia/cobalt/cpp/fidl.h>

#include "garnet/bin/cobalt/app/timer_manager.h"
#include "third_party/cobalt/logger/logger.h"

namespace cobalt {

class LoggerImpl : public fuchsia::cobalt::Logger,
                   public fuchsia::cobalt::LoggerSimple {
 public:
  LoggerImpl(std::unique_ptr<logger::ProjectContext> project_context,
             logger::Encoder* encoder,
             logger::ObservationWriter* observation_writer,
             TimerManager* timer_manager);

 private:
  void LogEvent(
      uint32_t metric_id, uint32_t event_type_index,
      fuchsia::cobalt::LoggerBase::LogEventCallback callback) override;

  void LogEventCount(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      int64_t period_duration_micros, int64_t count,
      fuchsia::cobalt::LoggerBase::LogEventCountCallback callback) override;

  void LogElapsedTime(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      int64_t elapsed_micros,
      fuchsia::cobalt::LoggerBase::LogElapsedTimeCallback callback) override;

  void LogFrameRate(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      float fps,
      fuchsia::cobalt::LoggerBase::LogFrameRateCallback callback) override;

  void LogMemoryUsage(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      int64_t bytes,
      fuchsia::cobalt::LoggerBase::LogMemoryUsageCallback callback) override;

  void LogString(
      uint32_t metric_id, fidl::StringPtr s,
      fuchsia::cobalt::LoggerBase::LogStringCallback callback) override;

  void LogIntHistogram(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      fidl::VectorPtr<fuchsia::cobalt::HistogramBucket> histogram,
      fuchsia::cobalt::Logger::LogIntHistogramCallback callback) override;

  void LogIntHistogram(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      fidl::VectorPtr<uint32_t> bucket_indices,
      fidl::VectorPtr<uint64_t> bucket_counts,
      fuchsia::cobalt::LoggerSimple::LogIntHistogramCallback callback) override;

  void LogCustomEvent(
      uint32_t metric_id,
      fidl::VectorPtr<fuchsia::cobalt::CustomEventValue> event_values,
      fuchsia::cobalt::Logger::LogCustomEventCallback callback) override;

  template <class CB>
  void AddTimerObservationIfReady(std::unique_ptr<TimerVal> timer_val_ptr,
                                  CB callback);

  void StartTimer(
      uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
      fidl::StringPtr timer_id, uint64_t timestamp, uint32_t timeout_s,
      fuchsia::cobalt::LoggerBase::StartTimerCallback callback) override;

  void EndTimer(
      fidl::StringPtr timer_id, uint64_t timestamp, uint32_t timeout_s,
      fuchsia::cobalt::LoggerBase::EndTimerCallback callback) override;

 private:
  std::unique_ptr<logger::ProjectContext> project_context_;
  logger::Logger logger_;
  TimerManager* timer_manager_;
};

}  // namespace cobalt

#endif  // GARNET_BIN_COBALT_APP_LOGGER_IMPL_H_
