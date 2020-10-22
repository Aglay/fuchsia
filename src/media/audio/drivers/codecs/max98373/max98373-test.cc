// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "max98373.h"

#include <lib/fake_ddk/fake_ddk.h>
#include <lib/mock-i2c/mock-i2c.h>
#include <lib/simple-codec/simple-codec-client.h>
#include <lib/simple-codec/simple-codec-helper.h>

#include <thread>

#include <mock/ddktl/protocol/gpio.h>
#include <zxtest/zxtest.h>

namespace audio {

struct Max98373Codec : public Max98373 {
  explicit Max98373Codec(const ddk::I2cChannel& i2c, const ddk::GpioProtocolClient& codec_reset)
      : Max98373(fake_ddk::kFakeParent, i2c, codec_reset) {}
  codec_protocol_t GetProto() { return {&this->codec_protocol_ops_, this}; }
};

TEST(Max98373Test, GetInfo) {
  mock_i2c::MockI2c unused_i2c;
  ddk::GpioProtocolClient unused_gpio;
  auto codec =
      SimpleCodecServer::Create<Max98373Codec>(unused_i2c.GetProto(), std::move(unused_gpio));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  {
    std::thread t([&]() {
      auto info = client.GetInfo();
      EXPECT_EQ(info.value().unique_id.compare(""), 0);
      EXPECT_EQ(info.value().manufacturer.compare("Maxim"), 0);
      EXPECT_EQ(info.value().product_name.compare("MAX98373"), 0);
    });
    t.join();
  }
}

TEST(Max98373Test, Reset) {
  mock_i2c::MockI2c mock_i2c;

  mock_i2c
      .ExpectWriteStop({0x20, 0x00, 0x01})  // Reset.
      .ExpectWrite({0x21, 0xff})
      .ExpectReadStop({0x43})                // Get revision id.
      .ExpectWriteStop({0x20, 0xff, 0x01})   // Global enable.
      .ExpectWriteStop({0x20, 0x43, 0x01})   // Speaker enable.
      .ExpectWriteStop({0x20, 0x3d, 0x28})   // Set gain to -20dB.
      .ExpectWriteStop({0x20, 0x2b, 0x01})   // Data in enable.
      .ExpectWriteStop({0x20, 0x24, 0xc0})   // I2S.
      .ExpectWriteStop({0x20, 0x27, 0x08});  // 48KHz.

  ddk::MockGpio mock_gpio;
  mock_gpio.ExpectWrite(ZX_OK, 0).ExpectWrite(ZX_OK, 1);  // Reset, set to 0 and then to 1.
  ddk::GpioProtocolClient gpio(mock_gpio.GetProto());
  auto codec = SimpleCodecServer::Create<Max98373Codec>(mock_i2c.GetProto(), std::move(gpio));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  // Delay to test we don't do other init I2C writes in another thread.
  zx::nanosleep(zx::deadline_after(zx::msec(100)));
  std::thread t([&]() { ASSERT_OK(client.Reset()); });
  t.join();
  mock_i2c.VerifyAndClear();
}

TEST(Max98373Test, SetGainGood) {
  mock_i2c::MockI2c mock_i2c;

  mock_i2c.ExpectWriteStop({0x20, 0x3d, 0x40});  // -32dB.

  ddk::GpioProtocolClient unused_gpio;
  auto codec =
      SimpleCodecServer::Create<Max98373Codec>(mock_i2c.GetProto(), std::move(unused_gpio));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  std::thread t([&]() {
    GainState gain({.gain_db = -32.f, .muted = false, .agc_enable = false});
    client.SetGainState(gain);
  });
  t.join();
  mock_i2c.VerifyAndClear();
}

TEST(Max98373Test, SetGainOurOfRangeLow) {
  mock_i2c::MockI2c mock_i2c;

  mock_i2c.ExpectWriteStop({0x20, 0x3d, 0x7f});  // -63.5dB.

  ddk::GpioProtocolClient unused_gpio;
  auto codec =
      SimpleCodecServer::Create<Max98373Codec>(mock_i2c.GetProto(), std::move(unused_gpio));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  std::thread t([&]() {
    GainState gain({.gain_db = -999.f, .muted = false, .agc_enable = false});
    client.SetGainState(gain);
  });
  t.join();
  mock_i2c.VerifyAndClear();
}

TEST(Max98373Test, SetGainOurOfRangeHigh) {
  mock_i2c::MockI2c mock_i2c;

  mock_i2c.ExpectWriteStop({0x20, 0x3d, 0x00});  // 0dB.

  ddk::GpioProtocolClient unused_gpio;
  auto codec =
      SimpleCodecServer::Create<Max98373Codec>(mock_i2c.GetProto(), std::move(unused_gpio));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  std::thread t([&]() {
    GainState gain({.gain_db = 999.f, .muted = false, .agc_enable = false});
    client.SetGainState(gain);
  });
  t.join();
  mock_i2c.VerifyAndClear();
}

}  // namespace audio
