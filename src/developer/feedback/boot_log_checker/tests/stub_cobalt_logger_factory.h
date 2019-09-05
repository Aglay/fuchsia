// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_BOOT_LOG_CHECKER_TESTS_STUB_COBALT_LOGGER_FACTORY_H_
#define SRC_DEVELOPER_FEEDBACK_BOOT_LOG_CHECKER_TESTS_STUB_COBALT_LOGGER_FACTORY_H_

#include <fuchsia/cobalt/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>

#include <memory>

#include "src/lib/fxl/logging.h"

namespace feedback {

class StubCobaltLoggerFactory : public fuchsia::cobalt::LoggerFactory {
 public:
  enum FailureMode { SUCCEED, FAIL_CLOSE_CONNECTIONS, FAIL_CREATE_LOGGER, FAIL_LOG_EVENT };
  explicit StubCobaltLoggerFactory(FailureMode failure_mode = SUCCEED)
      : failure_mode_(failure_mode), logger_(this) {}

  // Returns a request handler for binding to this stub service.
  fidl::InterfaceRequestHandler<fuchsia::cobalt::LoggerFactory> GetHandler() {
    return factory_bindings_.GetHandler(this);
  }

  uint32_t last_metric_id() { return logger_.last_metric_id(); }
  uint32_t last_event_code() { return logger_.last_event_code(); }

 private:
  class StubLogger : public fuchsia::cobalt::Logger {
   public:
    explicit StubLogger(StubCobaltLoggerFactory* factory) : factory_(factory) {}
    uint32_t last_metric_id() const { return last_metric_id_; }
    uint32_t last_event_code() const { return last_event_code_; }

   private:
    void LogEvent(uint32_t metric_id, uint32_t event_code,
                  fuchsia::cobalt::Logger::LogEventCallback callback) override;

    void LogEventCount(uint32_t metric_id, uint32_t event_code, ::std::string component,
                       int64_t period_duration_micros, int64_t count,
                       fuchsia::cobalt::Logger::LogEventCountCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogElapsedTime(uint32_t metric_id, uint32_t event_code, ::std::string component,
                        int64_t elapsed_micros,
                        fuchsia::cobalt::Logger::LogElapsedTimeCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogFrameRate(uint32_t metric_id, uint32_t event_code, ::std::string component, float fps,
                      fuchsia::cobalt::Logger::LogFrameRateCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogMemoryUsage(uint32_t metric_id, uint32_t event_code, ::std::string component,
                        int64_t bytes,
                        fuchsia::cobalt::Logger::LogMemoryUsageCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogString(uint32_t metric_id, ::std::string s,
                   fuchsia::cobalt::Logger::LogStringCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void StartTimer(uint32_t metric_id, uint32_t event_code, ::std::string component,
                    ::std::string timer_id, uint64_t timestamp, uint32_t timeout_s,
                    fuchsia::cobalt::Logger::StartTimerCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void EndTimer(::std::string timer_id, uint64_t timestamp, uint32_t timeout_s,
                  fuchsia::cobalt::Logger::EndTimerCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogIntHistogram(uint32_t metric_id, uint32_t event_code, ::std::string component,
                         ::std::vector<fuchsia::cobalt::HistogramBucket> histogram,
                         fuchsia::cobalt::Logger::LogIntHistogramCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogCustomEvent(uint32_t metric_id,
                        ::std::vector<fuchsia::cobalt::CustomEventValue> event_values,
                        fuchsia::cobalt::Logger::LogCustomEventCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogCobaltEvent(fuchsia::cobalt::CobaltEvent event,
                        fuchsia::cobalt::Logger::LogCobaltEventCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    void LogCobaltEvents(::std::vector<fuchsia::cobalt::CobaltEvent> events,
                         fuchsia::cobalt::Logger::LogCobaltEventsCallback callback) override {
      FXL_NOTIMPLEMENTED();
    }

    uint32_t last_metric_id_;
    uint32_t last_event_code_;
    StubCobaltLoggerFactory* factory_;
  };

  void CreateLoggerFromProjectName(
      ::std::string project_name, fuchsia::cobalt::ReleaseStage release_stage,
      ::fidl::InterfaceRequest<fuchsia::cobalt::Logger> logger,
      fuchsia::cobalt::LoggerFactory::CreateLoggerFromProjectNameCallback callback) override;
  void CreateLogger(fuchsia::cobalt::ProjectProfile profile,
                    ::fidl::InterfaceRequest<fuchsia::cobalt::Logger> logger,
                    fuchsia::cobalt::LoggerFactory::CreateLoggerCallback callback) override {
    FXL_NOTIMPLEMENTED();
  }
  void CreateLoggerSimple(
      fuchsia::cobalt::ProjectProfile profile,
      ::fidl::InterfaceRequest<fuchsia::cobalt::LoggerSimple> logger,
      fuchsia::cobalt::LoggerFactory::CreateLoggerSimpleCallback callback) override {
    FXL_NOTIMPLEMENTED();
  }
  void CreateLoggerSimpleFromProjectName(
      ::std::string project_name, fuchsia::cobalt::ReleaseStage release_stage,
      ::fidl::InterfaceRequest<fuchsia::cobalt::LoggerSimple> logger,
      fuchsia::cobalt::LoggerFactory::CreateLoggerSimpleFromProjectNameCallback callback) override {
    FXL_NOTIMPLEMENTED();
  }

 protected:
  void CloseAllConnections() {
    logger_bindings_.CloseAll();
    factory_bindings_.CloseAll();
  }

 private:
  fidl::BindingSet<fuchsia::cobalt::LoggerFactory> factory_bindings_;
  fidl::BindingSet<fuchsia::cobalt::Logger> logger_bindings_;
  FailureMode failure_mode_;
  StubLogger logger_;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_BOOT_LOG_CHECKER_TESTS_STUB_COBALT_LOGGER_FACTORY_H_
