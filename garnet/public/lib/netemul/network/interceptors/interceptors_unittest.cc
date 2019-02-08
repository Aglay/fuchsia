// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/gtest/real_loop_fixture.h"
#include "packet_loss.h"

namespace netemul {
namespace testing {

uint8_t gNextRandomNumber = 0;
uint8_t TestRNG() { return gNextRandomNumber; }

class InterceptorsTest : public gtest::RealLoopFixture {
 public:
  InterceptPacket MakeSingleBytePacket(uint8_t value) {
    return InterceptPacket(std::vector<uint8_t>({value}),
                           data::Consumer::Ptr());
  }
};

TEST_F(InterceptorsTest, PacketLossRealRand) {
  int half_loss_count = 0;
  interceptor::PacketLoss half_loss(
      50, [&half_loss_count](InterceptPacket packet) { half_loss_count++; });
  int full_loss_count = 0;
  interceptor::PacketLoss full_loss(
      100, [&full_loss_count](InterceptPacket packet) { full_loss_count++; });
  int no_loss_count = 0;
  interceptor::PacketLoss no_loss(
      0, [&no_loss_count](InterceptPacket packet) { no_loss_count++; });

  for (int i = 0; i < 500; i++) {
    half_loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(i)));
    full_loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(i)));
    no_loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(i)));
  }

  // full loss should have passed no packets
  EXPECT_EQ(full_loss_count, 0);
  // no loss should have passed all packets
  EXPECT_EQ(no_loss_count, 500);

  std::cout << half_loss_count << " packets passed at 50% loss" << std::endl;
  // expect that something around 250 packets should pass at 50% loss
  // give it wiggle room to prevent test being flaky
  EXPECT_TRUE(half_loss_count > 200 && half_loss_count < 300);
}

TEST_F(InterceptorsTest, PacketLossControlledRand) {
  int pass_count = 0;
  interceptor::PacketLoss<TestRNG> loss(
      50, [&pass_count](InterceptPacket packet) { pass_count++; });

  gNextRandomNumber = 99;
  loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(1)));
  EXPECT_EQ(pass_count, 1);

  gNextRandomNumber = 0;
  loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(1)));
  EXPECT_EQ(pass_count, 1);

  gNextRandomNumber = 50;
  loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(1)));
  EXPECT_EQ(pass_count, 2);

  gNextRandomNumber = 49;
  loss.Intercept(MakeSingleBytePacket(static_cast<uint8_t>(1)));
  EXPECT_EQ(pass_count, 2);
}

}  // namespace testing
}  // namespace netemul