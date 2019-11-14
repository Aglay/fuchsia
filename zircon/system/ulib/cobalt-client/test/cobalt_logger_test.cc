// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cobalt/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/loop.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <limits.h>
#include <zircon/assert.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cobalt-client/cpp/collector-internal.h>
#include <cobalt-client/cpp/in-memory-logger.h>
#include <cobalt-client/cpp/types-internal.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <zxtest/zxtest.h>

namespace cobalt_client {
namespace internal {
namespace {

using Status = ::llcpp::fuchsia::cobalt::Status;
using EventData = ::llcpp::fuchsia::cobalt::EventPayload;

// Fake Implementation for fuchsia::cobalt::LoggerFactory.
class FakeLoggerFactoryService : public ::llcpp::fuchsia::cobalt::LoggerFactory::Interface {
 public:
  void CreateLogger(::llcpp::fuchsia::cobalt::ProjectProfile profile, ::zx::channel logger,
                    CreateLoggerCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void CreateLoggerSimple(::llcpp::fuchsia::cobalt::ProjectProfile profile, ::zx::channel logger,
                          CreateLoggerSimpleCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void CreateLoggerFromProjectName(::fidl::StringView project_name,
                                   ::llcpp::fuchsia::cobalt::ReleaseStage release_stage,
                                   ::zx::channel logger,
                                   CreateLoggerFromProjectNameCompleter::Sync completer) final {
    completer.Reply(create_logger_handler_(project_name, release_stage, std::move(logger)));
  }

  void CreateLoggerSimpleFromProjectName(
      ::fidl::StringView project_name, ::llcpp::fuchsia::cobalt::ReleaseStage release_stage,
      ::zx::channel logger, CreateLoggerSimpleFromProjectNameCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void set_create_logger_handler(
      fit::function<Status(::fidl::StringView, ::llcpp::fuchsia::cobalt::ReleaseStage, zx::channel)>
          handler) {
    create_logger_handler_ = std::move(handler);
  }

 private:
  fit::function<Status(::fidl::StringView, ::llcpp::fuchsia::cobalt::ReleaseStage, zx::channel)>
      create_logger_handler_;
};

// Fake Implementation for fuchsia::cobalt::Logger.
class FakeLoggerService : public ::llcpp::fuchsia::cobalt::Logger::Interface {
 public:
  void LogEvent(uint32_t metric_id, uint32_t event_code, LogEventCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void LogEventCount(uint32_t metric_id, uint32_t event_code, ::fidl::StringView component,
                     int64_t period_duration_micros, int64_t count,
                     LogEventCountCompleter::Sync completer) {
    ZX_PANIC("Not Implemented.");
  }

  void LogElapsedTime(uint32_t metric_id, uint32_t event_code, ::fidl::StringView component,
                      int64_t elapsed_micros, LogElapsedTimeCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void LogFrameRate(uint32_t metric_id, uint32_t event_code, ::fidl::StringView component,
                    float fps, LogFrameRateCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void LogMemoryUsage(uint32_t metric_id, uint32_t event_code, ::fidl::StringView component,
                      int64_t bytes, LogMemoryUsageCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void LogString(uint32_t metric_id, ::fidl::StringView s,
                 LogStringCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void StartTimer(uint32_t metric_id, uint32_t event_code, ::fidl::StringView component,
                  ::fidl::StringView timer_id, uint64_t timestamp, uint32_t timeout_s,
                  StartTimerCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void EndTimer(::fidl::StringView timer_id, uint64_t timestamp, uint32_t timeout_s,
                EndTimerCompleter::Sync completer) {
    ZX_PANIC("Not Implemented.");
  }

  void LogIntHistogram(uint32_t metric_id, uint32_t event_code, ::fidl::StringView component,
                       ::fidl::VectorView<::llcpp::fuchsia::cobalt::HistogramBucket> histogram,
                       LogIntHistogramCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void LogCustomEvent(uint32_t metric_id,
                      ::fidl::VectorView<::llcpp::fuchsia::cobalt::CustomEventValue> event_values,
                      LogCustomEventCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void LogCobaltEvent(::llcpp::fuchsia::cobalt::CobaltEvent event,
                      LogCobaltEventCompleter::Sync completer) final {
    // Use MetricInfo as a key.
    MetricInfo info;
    info.metric_id = event.metric_id;
    info.component = event.component.data();
    for (uint64_t i = 0; i < MetricInfo::kMaxEventCodes; ++i) {
      info.event_codes[i] = event.event_codes[i];
    }
    switch (event.payload.which()) {
      case EventData::Tag::kIntHistogram:
        storage_.Log(info, event.payload.int_histogram().data(),
                     event.payload.int_histogram().count());
        break;
      case EventData::Tag::kEventCount:
        storage_.Log(info, event.payload.event_count().count);
        break;
      default:
        ZX_ASSERT_MSG(false, "Not Supported.");
        break;
    }
    completer.Reply(log_return_status_);
  }

  void LogCobaltEvents(::fidl::VectorView<::llcpp::fuchsia::cobalt::CobaltEvent> events,
                       LogCobaltEventsCompleter::Sync completer) final {
    ZX_PANIC("Not Implemented.");
  }

  void set_log_return_status(Status status) { log_return_status_ = status; }

  // Returns the |InMemoryLogger| used for backing the storage of this |cobalt.Logger|.
  const InMemoryLogger& storage() const { return storage_; }

 private:
  Status log_return_status_ = Status::OK;
  InMemoryLogger storage_;
};

// Struct for argument validation.
struct CreateLoggerValidationArgs {
  void Check() const {
    EXPECT_TRUE(is_name_ok);
    EXPECT_TRUE(is_stage_ok);
    EXPECT_TRUE(is_channel_ok);
  }

  std::string project_name;
  ::llcpp::fuchsia::cobalt::ReleaseStage stage;

  // Return status for the fidl call.
  Status return_status = Status::OK;

  // Used for validating the args and validation on the main thread.
  fbl::Mutex result_lock_;
  bool is_name_ok = false;
  bool is_stage_ok = false;
  bool is_channel_ok = false;
};

void BindLoggerFactoryService(FakeLoggerFactoryService* bindee, zx::channel channel,
                              async_dispatcher_t* dispatcher) {
  fidl::Bind(dispatcher, std::move(channel), bindee);
}

void BindLoggerToLoggerFactoryService(FakeLoggerFactoryService* binder, FakeLoggerService* bindee,
                                      CreateLoggerValidationArgs* checker,
                                      async_dispatcher_t* dispatcher) {
  binder->set_create_logger_handler([bindee, checker, dispatcher](
                                        ::fidl::StringView project_name,
                                        ::llcpp::fuchsia::cobalt::ReleaseStage stage,
                                        zx::channel channel) {
    fbl::AutoLock lock(&checker->result_lock_);
    checker->is_name_ok =
        (checker->project_name == std::string_view(project_name.data(), project_name.size()));
    checker->is_stage_ok = (static_cast<std::underlying_type<ReleaseStage>::type>(checker->stage) ==
                            static_cast<std::underlying_type<ReleaseStage>::type>(stage));
    checker->is_channel_ok = channel.is_valid();
    fidl::Bind(dispatcher, std::move(channel), bindee);

    return checker->return_status;
  });
}

constexpr std::string_view kProjectName = "SomeProject";
constexpr ReleaseStage kReleaseStage = ReleaseStage::kGa;

class LoggerServiceFixture : public zxtest::Test {
 public:
  void SetUp() final {
    // Initialize the service loop.
    service_loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigNoAttachToThread);

    checker_.project_name = kProjectName;
    checker_.stage = static_cast<decltype(checker_.stage)>(kReleaseStage);
    checker_.return_status = Status::OK;

    // Set up logger factory service.
    CobaltOptions options;
    options.project_name = kProjectName;
    options.release_stage = kReleaseStage;
    options.service_connect = [this](const char* path, zx::channel service_channel) {
      BindLoggerFactoryService(&logger_factory_impl_, std::move(service_channel),
                               service_loop_->dispatcher());
      return ZX_OK;
    };
    logger_ = std::make_unique<CobaltLogger>(std::move(options));

    BindLoggerToLoggerFactoryService(&logger_factory_impl_, &logger_impl_, &checker_,
                                     service_loop_->dispatcher());
  }

  void StartServiceLoop() {
    ASSERT_NOT_NULL(service_loop_);
    ASSERT_TRUE(service_loop_->GetState() == ASYNC_LOOP_RUNNABLE);
    service_loop_->StartThread("LoggerServiceThread");
  }

  void StopServiceLoop() {
    service_loop_->Quit();
    service_loop_->JoinThreads();
    service_loop_->ResetQuit();
  }

  void TearDown() final { StopServiceLoop(); }

  const InMemoryLogger& GetStorage() const { return logger_impl_.storage(); }

  async::Loop* GetLoop() { return service_loop_.get(); }

  Logger* logger() { return logger_.get(); }

  void SetLoggerLogReturnStatus(Status status) { logger_impl_.set_log_return_status(status); }

 protected:
  CreateLoggerValidationArgs checker_;

 private:
  std::unique_ptr<CobaltLogger> logger_ = nullptr;

  std::unique_ptr<async::Loop> service_loop_ = nullptr;

  FakeLoggerFactoryService logger_factory_impl_;
  FakeLoggerService logger_impl_;
};

using CobaltLoggerTest = LoggerServiceFixture;

constexpr uint64_t kBucketCount = 10;

TEST_F(CobaltLoggerTest, LogHistogramReturnsTrueWhenServiceReturnsOk) {
  std::vector<HistogramBucket> buckets;

  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};

  for (uint32_t i = 0; i < kBucketCount; ++i) {
    buckets.push_back({.index = i, .count = 2 * i});
  }

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  ASSERT_TRUE(logger()->Log(info, buckets.data(), buckets.size()));
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
  auto itr = GetStorage().histograms().find(info);
  ASSERT_NE(GetStorage().histograms().end(), itr);
  ASSERT_EQ(itr->second.size(), kBucketCount);

  for (uint32_t i = 0; i < itr->second.size(); ++i) {
    EXPECT_EQ(buckets[i].count, (itr->second).at(i));
  }
}

TEST_F(CobaltLoggerTest, LogHistogramReturnsFalseWhenFactoryServiceReturnsError) {
  std::vector<HistogramBucket> buckets;

  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};
  checker_.return_status = Status::INTERNAL_ERROR;

  for (uint32_t i = 0; i < kBucketCount; ++i) {
    buckets.push_back({.index = i, .count = 2 * i});
  }

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  ASSERT_FALSE(logger()->Log(info, buckets.data(), buckets.size()));
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
  EXPECT_TRUE(GetStorage().histograms().empty());
  EXPECT_TRUE(GetStorage().counters().empty());
}

TEST_F(CobaltLoggerTest, LogHistogramReturnsFalseWhenLoggerServiceReturnsError) {
  std::vector<HistogramBucket> buckets;

  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};
  SetLoggerLogReturnStatus(Status::INTERNAL_ERROR);

  for (uint32_t i = 0; i < kBucketCount; ++i) {
    buckets.push_back({.index = i, .count = 2 * i});
  }

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  ASSERT_FALSE(logger()->Log(info, buckets.data(), buckets.size()));
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
}

TEST_F(CobaltLoggerTest, LogHistogramWaitsUntilServiceBecomesAvailable) {
  std::vector<HistogramBucket> buckets;
  std::atomic<bool> log_result(false);

  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};

  for (uint32_t i = 0; i < kBucketCount; ++i) {
    buckets.push_back({.index = i, .count = 2 * i});
  }

  std::thread blocks_until_starts(
      [info, &log_result, &buckets](internal::Logger* logger) {
        log_result.store(logger->Log(info, buckets.data(), buckets.size()));
      },
      logger());

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  // This should wait until Log finishes.
  blocks_until_starts.join();

  ASSERT_TRUE(log_result);
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
  auto itr = GetStorage().histograms().find(info);
  ASSERT_NE(GetStorage().histograms().end(), itr);
  ASSERT_EQ(itr->second.size(), kBucketCount);

  for (uint32_t i = 0; i < itr->second.size(); ++i) {
    EXPECT_EQ(buckets[i].count, (itr->second).at(i));
  }
}

constexpr int64_t kCounter = 1;

TEST_F(CobaltLoggerTest, LogCounterReturnsTrueWhenServiceReturnsOk) {
  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  ASSERT_TRUE(logger()->Log(info, kCounter));
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
  auto itr = GetStorage().counters().find(info);
  ASSERT_NE(GetStorage().counters().end(), itr);

  EXPECT_EQ(itr->second, kCounter);
}

TEST_F(CobaltLoggerTest, LogCounterReturnsFalseWhenFactoryServiceReturnsError) {
  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};
  checker_.return_status = Status::INTERNAL_ERROR;

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  ASSERT_FALSE(logger()->Log(info, kCounter));
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
  EXPECT_TRUE(GetStorage().histograms().empty());
  EXPECT_TRUE(GetStorage().counters().empty());
}

TEST_F(CobaltLoggerTest, LogCounterReturnsFalseWhenLoggerServiceReturnsError) {
  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};
  SetLoggerLogReturnStatus(Status::INTERNAL_ERROR);

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  ASSERT_FALSE(logger()->Log(info, kCounter));
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
}

TEST_F(CobaltLoggerTest, LogCounterWaitsUntilServiceBecomesAvailable) {
  std::atomic<bool> log_result(false);
  MetricInfo info;
  info.metric_id = 1;
  info.component = "SomeComponent";
  info.event_codes = {1, 2, 3, 4, 5};

  std::thread blocks_until_starts(
      [info, &log_result](internal::Logger* logger) {
        log_result.store(logger->Log(info, kCounter));
      },
      logger());

  ASSERT_NO_FAILURES(StartServiceLoop(), "Failed to initialize the service async dispatchers.");

  // This should wait until Log finishes.
  blocks_until_starts.join();

  ASSERT_TRUE(log_result.load());
  ASSERT_NO_FATAL_FAILURES(checker_.Check());
  auto itr = GetStorage().counters().find(info);
  ASSERT_NE(GetStorage().counters().end(), itr);

  EXPECT_EQ(itr->second, kCounter);
}

}  // namespace
}  // namespace internal
}  // namespace cobalt_client
