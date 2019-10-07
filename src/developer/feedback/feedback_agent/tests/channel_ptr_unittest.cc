// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/channel_ptr.h"

#include <lib/async/cpp/executor.h>
#include <lib/fit/single_threaded_executor.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <memory>
#include <string>

#include "src/developer/feedback/feedback_agent/tests/stub_channel_provider.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/test/test_settings.h"
#include "src/lib/syslog/cpp/logger.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

class RetrieveCurrentChannelTest : public gtest::TestLoopFixture {
 public:
  RetrieveCurrentChannelTest()
      : executor_(dispatcher()), service_directory_provider_(dispatcher()) {}

 protected:
  void SetUpChannelProvider(std::unique_ptr<StubChannelProvider> stub_channel_provider) {
    stub_channel_provider_ = std::move(stub_channel_provider);
    if (stub_channel_provider_) {
      FXL_CHECK(service_directory_provider_.AddService(stub_channel_provider_->GetHandler()) ==
                ZX_OK);
    }
  }

  fit::result<std::string> RetrieveCurrentChannel(const zx::duration timeout = zx::sec(1)) {
    fit::result<std::string> result;
    executor_.schedule_task(
        feedback::RetrieveCurrentChannel(dispatcher(),
                                         service_directory_provider_.service_directory(), timeout)
            .then([&result](fit::result<std::string>& res) { result = std::move(res); }));
    RunLoopFor(timeout);
    return result;
  }

  async::Executor executor_;
  sys::testing::ServiceDirectoryProvider service_directory_provider_;

 private:
  std::unique_ptr<StubChannelProvider> stub_channel_provider_;
};

TEST_F(RetrieveCurrentChannelTest, Succeed_SomeChannel) {
  std::unique_ptr<StubChannelProvider> stub_channel_provider =
      std::make_unique<StubChannelProvider>();
  stub_channel_provider->set_channel("my-channel");
  SetUpChannelProvider(std::move(stub_channel_provider));

  fit::result<std::string> result = RetrieveCurrentChannel();

  ASSERT_TRUE(result.is_ok());
  EXPECT_STREQ(result.take_value().c_str(), "my-channel");
}

TEST_F(RetrieveCurrentChannelTest, Succeed_EmptyChannel) {
  SetUpChannelProvider(std::make_unique<StubChannelProvider>());

  fit::result<std::string> result = RetrieveCurrentChannel();

  ASSERT_TRUE(result.is_ok());
  EXPECT_STREQ(result.take_value().c_str(), "");
}

TEST_F(RetrieveCurrentChannelTest, Fail_ChannelProviderNotAvailable) {
  SetUpChannelProvider(nullptr);

  fit::result<std::string> result = RetrieveCurrentChannel();

  ASSERT_TRUE(result.is_error());
}

TEST_F(RetrieveCurrentChannelTest, Fail_ChannelProviderClosesConnection) {
  SetUpChannelProvider(std::make_unique<StubChannelProviderClosesConnection>());

  fit::result<std::string> result = RetrieveCurrentChannel();

  ASSERT_TRUE(result.is_error());
}

TEST_F(RetrieveCurrentChannelTest, Fail_ChannelProviderNeverReturns) {
  SetUpChannelProvider(std::make_unique<StubChannelProviderNeverReturns>());

  fit::result<std::string> result = RetrieveCurrentChannel();

  ASSERT_TRUE(result.is_error());
}

TEST_F(RetrieveCurrentChannelTest, Fail_CallGetCurrentTwice) {
  SetUpChannelProvider(std::make_unique<StubChannelProvider>());

  const zx::duration unused_timeout = zx::sec(1);
  ChannelProvider channel_provider(dispatcher(), service_directory_provider_.service_directory());
  executor_.schedule_task(channel_provider.GetCurrent(unused_timeout));
  ASSERT_DEATH(channel_provider.GetCurrent(unused_timeout),
               testing::HasSubstr("GetCurrent() is not intended to be called twice"));
}

}  // namespace
}  // namespace feedback

int main(int argc, char** argv) {
  if (!fxl::SetTestSettings(argc, argv)) {
    return EXIT_FAILURE;
  }

  testing::InitGoogleTest(&argc, argv);
  syslog::InitLogger({"feedback", "test"});
  return RUN_ALL_TESTS();
}
