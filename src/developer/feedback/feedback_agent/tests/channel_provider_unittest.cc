// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/annotations/channel_provider.h"

#include <lib/async/cpp/executor.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <memory>
#include <optional>
#include <string>

#include "src/developer/feedback/feedback_agent/tests/stub_channel_provider.h"
#include "src/developer/feedback/testing/unit_test_fixture.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/test/test_settings.h"
#include "src/lib/syslog/cpp/logger.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

using fuchsia::feedback::Annotation;

class ChannelProviderTest : public UnitTestFixture {
 public:
  ChannelProviderTest() : executor_(dispatcher()) {}

 protected:
  void SetUpChannelProviderPtr(std::unique_ptr<StubChannelProvider> channel_provider) {
    channel_provider_ = std::move(channel_provider);
    if (channel_provider_) {
      InjectServiceProvider(channel_provider_.get());
    }
  }

  std::optional<std::string> RetrieveCurrentChannel(const zx::duration timeout = zx::sec(1)) {
    std::optional<std::string> channel;
    ChannelProvider provider(dispatcher(), services(), timeout);
    auto promises = provider.GetAnnotations();
    executor_.schedule_task(
        std::move(promises).then([&channel](fit::result<std::vector<Annotation>>& res) {
          if (res.is_error()) {
            channel = std::nullopt;
          } else {
            std::vector<Annotation> vec = res.take_value();
            if (vec.empty()) {
              channel = std::nullopt;
            } else {
              FX_CHECK(vec.size() == 1u);
              channel = std::move(vec.back().value);
            }
          }
        }));
    RunLoopFor(timeout);
    return channel;
  }

  async::Executor executor_;

 private:
  std::unique_ptr<StubChannelProvider> channel_provider_;
};

TEST_F(ChannelProviderTest, Succeed_SomeChannel) {
  auto channel_provider = std::make_unique<StubChannelProvider>();
  channel_provider->set_channel("my-channel");
  SetUpChannelProviderPtr(std::move(channel_provider));

  const auto result = RetrieveCurrentChannel();

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), "my-channel");
}

TEST_F(ChannelProviderTest, Succeed_EmptyChannel) {
  SetUpChannelProviderPtr(std::make_unique<StubChannelProvider>());

  const auto result = RetrieveCurrentChannel();

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), "");
}

TEST_F(ChannelProviderTest, Fail_ChannelProviderPtrNotAvailable) {
  SetUpChannelProviderPtr(nullptr);

  const auto result = RetrieveCurrentChannel();

  ASSERT_FALSE(result);
}

TEST_F(ChannelProviderTest, Fail_ChannelProviderPtrClosesConnection) {
  SetUpChannelProviderPtr(std::make_unique<StubChannelProviderClosesConnection>());

  const auto result = RetrieveCurrentChannel();

  ASSERT_FALSE(result);
}

TEST_F(ChannelProviderTest, Fail_ChannelProviderPtrNeverReturns) {
  SetUpChannelProviderPtr(std::make_unique<StubChannelProviderNeverReturns>());

  const auto result = RetrieveCurrentChannel();

  ASSERT_FALSE(result);
}

TEST_F(ChannelProviderTest, Fail_CallGetCurrentTwice) {
  SetUpChannelProviderPtr(std::make_unique<StubChannelProvider>());

  const zx::duration unused_timeout = zx::sec(1);
  internal::ChannelProviderPtr channel_provider(dispatcher(), services());
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
