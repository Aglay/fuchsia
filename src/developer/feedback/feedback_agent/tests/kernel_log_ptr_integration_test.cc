// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/boot/c/fidl.h>
#include <fuchsia/boot/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/test_with_environment.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <memory>
#include <vector>

#include "src/developer/feedback/feedback_agent/attachments/aliases.h"
#include "src/developer/feedback/feedback_agent/attachments/kernel_log_ptr.h"
#include "src/developer/feedback/testing/stubs/cobalt_logger_factory.h"
#include "src/developer/feedback/utils/cobalt_metrics.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

using testing::UnorderedElementsAreArray;

class CollectKernelLogTest : public sys::testing::TestWithEnvironment {
 public:
  CollectKernelLogTest() : executor_(dispatcher()) {}

  void SetUp() override { environment_services_ = sys::ServiceDirectory::CreateFromNamespace(); }

  fit::result<AttachmentValue> GetKernelLog() {
    cobalt_ = std::make_unique<Cobalt>(dispatcher(), environment_services_);
    fit::result<AttachmentValue> result;
    const zx::duration timeout(zx::sec(10));
    bool done = false;
    executor_.schedule_task(
        CollectKernelLog(dispatcher(), environment_services_, timeout, cobalt_.get())
            .then([&result, &done](fit::result<AttachmentValue>& res) {
              result = std::move(res);
              done = true;
            }));
    RunLoopUntil([&done] { return done; });
    return result;
  }

 protected:
  std::shared_ptr<sys::ServiceDirectory> environment_services_;
  std::unique_ptr<Cobalt> cobalt_;
  async::Executor executor_;
};

void SendToKernelLog(const std::string& str) {
  zx::channel local, remote;
  ASSERT_EQ(zx::channel::create(0, &local, &remote), ZX_OK);
  constexpr char kWriteOnlyLogPath[] = "/svc/" fuchsia_boot_WriteOnlyLog_Name;
  ASSERT_EQ(fdio_service_connect(kWriteOnlyLogPath, remote.release()), ZX_OK);

  zx::debuglog log;
  ASSERT_EQ(fuchsia_boot_WriteOnlyLogGet(local.get(), log.reset_and_get_address()), ZX_OK);

  zx_debuglog_write(log.get(), 0, str.c_str(), str.size());
}

TEST_F(CollectKernelLogTest, Succeed_BasicCase) {
  const std::string output(
      fxl::StringPrintf("<<GetLogTest_Succeed_BasicCase: %zu>>", zx_clock_get_monotonic()));
  SendToKernelLog(output);

  fit::result<AttachmentValue> result = GetKernelLog();
  ASSERT_TRUE(result.is_ok());
  AttachmentValue logs = result.take_value();
  EXPECT_THAT(logs, testing::HasSubstr(output));
}

TEST_F(CollectKernelLogTest, Succeed_TwoRetrievals) {
  // ReadOnlyLog was returning a shared handle so the second reader would get data after where the
  // first had read from. Confirm that both readers get the target string.
  const std::string output(
      fxl::StringPrintf("<<GetLogTest_Succeed_TwoRetrievals: %zu>>", zx_clock_get_monotonic()));
  SendToKernelLog(output);

  fit::result<AttachmentValue> result = GetKernelLog();
  ASSERT_TRUE(result.is_ok());
  AttachmentValue logs = result.take_value();
  EXPECT_THAT(logs, testing::HasSubstr(output));

  fit::result<AttachmentValue> second_result = GetKernelLog();
  ASSERT_TRUE(second_result.is_ok());
  AttachmentValue second_logs = second_result.take_value();
  EXPECT_THAT(second_logs, testing::HasSubstr(output));
}

TEST_F(CollectKernelLogTest, Fail_CallGetLogTwice) {
  Cobalt cobalt(dispatcher(), environment_services_);
  const zx::duration unused_timeout = zx::sec(1);
  BootLog bootlog(dispatcher(), environment_services_, &cobalt);
  executor_.schedule_task(bootlog.GetLog(unused_timeout));
  ASSERT_DEATH(bootlog.GetLog(unused_timeout),
               testing::HasSubstr("GetLog() is not intended to be called twice"));
}

TEST_F(CollectKernelLogTest, Check_CobaltLogsTimeout) {
  auto services = CreateServices();
  stubs::CobaltLoggerFactory logger_factory;
  services->AddService(logger_factory.GetHandler());

  auto enclosing_environment = CreateNewEnclosingEnvironment(
      "kernel_log_ptr_integration_test_environment", std::move(services));

  Cobalt cobalt(dispatcher(), enclosing_environment->service_directory());

  // Set the timeout to 0 so kernel log collection always times out.
  const zx::duration timeout = zx::sec(0);
  BootLog bootlog(dispatcher(), enclosing_environment->service_directory(), &cobalt);
  executor_.schedule_task(bootlog.GetLog(timeout));

  // We don't control the loop so we need to make sure the Cobalt event is logged before checking
  // its value.
  RunLoopUntil([&logger_factory] { return logger_factory.Events().size() > 0u; });
  EXPECT_THAT(logger_factory.Events(), UnorderedElementsAreArray({
                                           CobaltEvent(TimedOutData::kKernelLog),
                                       }));
}

}  // namespace
}  // namespace feedback
