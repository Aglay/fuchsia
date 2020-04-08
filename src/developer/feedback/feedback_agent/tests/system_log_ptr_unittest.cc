// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/attachments/system_log_ptr.h"

#include <lib/async/cpp/executor.h>
#include <lib/syslog/logger.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <memory>
#include <ostream>
#include <vector>

#include "src/developer/feedback/feedback_agent/attachments/aliases.h"
#include "src/developer/feedback/testing/cobalt_test_fixture.h"
#include "src/developer/feedback/testing/gpretty_printers.h"
#include "src/developer/feedback/testing/stubs/cobalt_logger_factory.h"
#include "src/developer/feedback/testing/stubs/logger.h"
#include "src/developer/feedback/testing/unit_test_fixture.h"
#include "src/developer/feedback/utils/cobalt_event.h"
#include "src/developer/feedback/utils/cobalt_metrics.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/lib/fxl/logging.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

using testing::UnorderedElementsAreArray;

class CollectSystemLogTest : public UnitTestFixture, public CobaltTestFixture {
 public:
  CollectSystemLogTest() : CobaltTestFixture(/*unit_test_fixture=*/this), executor_(dispatcher()) {}

 protected:
  void SetUpLogger(std::unique_ptr<stubs::LoggerBase> logger) {
    logger_ = std::move(logger);
    if (logger_) {
      InjectServiceProvider(logger_.get());
    }
  }

  ::fit::result<AttachmentValue> CollectSystemLog(const zx::duration timeout = zx::sec(1)) {
    SetUpCobaltLoggerFactory(std::make_unique<stubs::CobaltLoggerFactory>());
    Cobalt cobalt(dispatcher(), services());

    ::fit::result<AttachmentValue> result;
    executor_.schedule_task(
        feedback::CollectSystemLog(dispatcher(), services(), timeout, &cobalt)
            .then([&result](::fit::result<AttachmentValue>& res) { result = std::move(res); }));
    RunLoopFor(timeout);
    return result;
  }

 private:
  async::Executor executor_;

  std::unique_ptr<stubs::LoggerBase> logger_;
};

TEST_F(CollectSystemLogTest, Succeed_BasicCase) {
  std::unique_ptr<stubs::Logger> logger = std::make_unique<stubs::Logger>();
  logger->set_messages({
      stubs::BuildLogMessage(FX_LOG_INFO, "line 1"),
      stubs::BuildLogMessage(FX_LOG_WARNING, "line 2", zx::msec(1)),
      stubs::BuildLogMessage(FX_LOG_ERROR, "line 3", zx::msec(2)),
      stubs::BuildLogMessage(FX_LOG_FATAL, "line 4", zx::msec(3)),
      stubs::BuildLogMessage(-1 /*VLOG(1)*/, "line 5", zx::msec(4)),
      stubs::BuildLogMessage(-2 /*VLOG(2)*/, "line 6", zx::msec(5)),
      stubs::BuildLogMessage(FX_LOG_INFO, "line 7", zx::msec(6), /*tags=*/{"foo"}),
      stubs::BuildLogMessage(FX_LOG_INFO, "line 8", zx::msec(7), /*tags=*/{"bar"}),
      stubs::BuildLogMessage(FX_LOG_INFO, "line 9", zx::msec(8),
                             /*tags=*/{"foo", "bar"}),
  });
  SetUpLogger(std::move(logger));

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_ok());
  AttachmentValue logs = result.take_value();
  EXPECT_STREQ(logs.c_str(),
               R"([15604.000][07559][07687][] INFO: line 1
[15604.001][07559][07687][] WARN: line 2
[15604.002][07559][07687][] ERROR: line 3
[15604.003][07559][07687][] FATAL: line 4
[15604.004][07559][07687][] VLOG(1): line 5
[15604.005][07559][07687][] VLOG(2): line 6
[15604.006][07559][07687][foo] INFO: line 7
[15604.007][07559][07687][bar] INFO: line 8
[15604.008][07559][07687][foo, bar] INFO: line 9
)");
}

TEST_F(CollectSystemLogTest, Succeed_LoggerUnbindsFromLogListenerAfterOneMessage) {
  auto logger = std::make_unique<stubs::LoggerUnbindsFromLogListenerAfterOneMessage>();
  logger->set_messages({
      stubs::BuildLogMessage(FX_LOG_INFO, "this line should appear in the partial logs"),
      stubs::BuildLogMessage(FX_LOG_INFO, "this line should be missing from the partial logs"),
  });
  SetUpLogger(std::move(logger));

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_ok());
  AttachmentValue logs = result.take_value();
  EXPECT_STREQ(logs.c_str(),
               "[15604.000][07559][07687][] INFO: this line should appear in the partial logs\n");
}

TEST_F(CollectSystemLogTest, Succeed_LogCollectionTimesOut) {
  // The logger will delay sending the rest of the messages after the first message.
  // The delay needs to be longer than the log collection timeout to get partial logs.
  // Since we are using a test loop with a fake clock, the actual durations don't matter so we can
  // set them arbitrary long.
  const zx::duration logger_delay = zx::sec(10);
  const zx::duration log_collection_timeout = zx::sec(1);

  auto logger = std::make_unique<stubs::LoggerDelaysAfterOneMessage>(dispatcher(), logger_delay);
  logger->set_messages({
      stubs::BuildLogMessage(FX_LOG_INFO, "this line should appear in the partial logs"),
      stubs::BuildLogMessage(FX_LOG_INFO, "this line should be missing from the partial logs"),
  });
  SetUpLogger(std::move(logger));

  ::fit::result<AttachmentValue> result = CollectSystemLog(log_collection_timeout);

  // First, we check that the log collection terminated with partial logs after the timeout.
  ASSERT_TRUE(result.is_ok());
  AttachmentValue logs = result.take_value();
  EXPECT_STREQ(logs.c_str(),
               "[15604.000][07559][07687][] INFO: this line should appear in the partial logs\n");

  // Then, we check that nothing crashes when the server tries to send the rest of the messages
  // after the connection has been lost.
  ASSERT_TRUE(RunLoopFor(logger_delay));
  EXPECT_THAT(ReceivedCobaltEvents(), UnorderedElementsAreArray({
                                          CobaltEvent(TimedOutData::kSystemLog),
                                      }));
}

TEST_F(CollectSystemLogTest, Fail_EmptyLog) {
  SetUpLogger(std::make_unique<stubs::Logger>());

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_error());
}

TEST_F(CollectSystemLogTest, Fail_LoggerNotAvailable) {
  SetUpLogger(nullptr);

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_error());
}

TEST_F(CollectSystemLogTest, Fail_LoggerClosesConnection) {
  SetUpLogger(std::make_unique<stubs::LoggerClosesConnection>());

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_error());
}

TEST_F(CollectSystemLogTest, Fail_LoggerNeverBindsToLogListener) {
  SetUpLogger(std::make_unique<stubs::LoggerNeverBindsToLogListener>());

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_error());
}

TEST_F(CollectSystemLogTest, Fail_LoggerNeverCallsLogManyBeforeDone) {
  SetUpLogger(std::make_unique<stubs::LoggerNeverCallsLogManyBeforeDone>());

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_error());
}

TEST_F(CollectSystemLogTest, Fail_LogCollectionTimesOut) {
  SetUpLogger(std::make_unique<stubs::LoggerBindsToLogListenerButNeverCalls>());

  ::fit::result<AttachmentValue> result = CollectSystemLog();

  ASSERT_TRUE(result.is_error());
}

class LogListenerTest : public UnitTestFixture, public CobaltTestFixture {
 public:
  LogListenerTest() : CobaltTestFixture(/*unit_test_fixture=*/this), executor_(dispatcher()) {}

 protected:
  async::Executor executor_;
};

// DX-1602
TEST_F(LogListenerTest, Succeed_LoggerClosesConnectionAfterSuccessfulFlow) {
  std::unique_ptr<stubs::Logger> logger = std::make_unique<stubs::Logger>();
  logger->set_messages({
      stubs::BuildLogMessage(FX_LOG_INFO, "msg"),
  });
  InjectServiceProvider(logger.get());

  SetUpCobaltLoggerFactory(std::make_unique<stubs::CobaltLoggerFactory>());
  Cobalt cobalt(dispatcher(), services());

  // Since we are using a test loop with a fake clock, the actual duration doesn't matter so we can
  // set it arbitrary long.
  const zx::duration timeout = zx::sec(1);
  ::fit::result<void> result;
  LogListener log_listener(dispatcher(), services());
  executor_.schedule_task(
      log_listener.CollectLogs(fit::Timeout(timeout))
          .then([&result](const ::fit::result<void>& res) { result = std::move(res); }));
  RunLoopFor(timeout);

  // First, we check we have had a successful flow.
  ASSERT_TRUE(result.is_ok());

  // Then, we check that if the logger closes the connection (and triggers the error handler on the
  // LogListener side), we don't crash (cf. DX-1602).
  logger->CloseConnection();
}

}  // namespace
}  // namespace feedback
