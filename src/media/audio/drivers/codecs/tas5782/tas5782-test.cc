// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5782.h"

#include <lib/fake_ddk/fake_ddk.h>
#include <lib/mock-i2c/mock-i2c.h>
#include <lib/simple-codec/simple-codec-client.h>
#include <lib/sync/completion.h>

#include <mock/ddktl/protocol/gpio.h>
#include <zxtest/zxtest.h>

namespace audio {

namespace {

audio::DaiFormat GetDefaultDaiFormat() {
  return {
      .number_of_channels = 2,
      .channels_to_use_bitmask = 3,
      .sample_format = SAMPLE_FORMAT_PCM_SIGNED,
      .frame_format = FRAME_FORMAT_I2S,
      .frame_rate = 48'000,
      .bits_per_slot = 32,
      .bits_per_sample = 32,
  };
}
}  // namespace

struct Tas5782Codec : public Tas5782 {
  explicit Tas5782Codec(const ddk::I2cChannel& i2c, const ddk::GpioProtocolClient& codec_reset,
                        const ddk::GpioProtocolClient& codec_mute)
      : Tas5782(fake_ddk::kFakeParent, i2c, codec_reset, codec_mute) {
    initialized_ = true;
  }
  codec_protocol_t GetProto() { return {&this->codec_protocol_ops_, this}; }
};

TEST(Tas5782Test, GoodSetDai) {
  mock_i2c::MockI2c mock_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;

  auto codec = SimpleCodecServer::Create<Tas5782Codec>(mock_i2c.GetProto(), std::move(unused_gpio0),
                                                       std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  DaiFormat format = GetDefaultDaiFormat();
  ASSERT_OK(client.SetDaiFormat(std::move(format)));

  mock_i2c.VerifyAndClear();
}

TEST(Tas5782Test, BadSetDai) {
  mock_i2c::MockI2c mock_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;

  auto codec = SimpleCodecServer::Create<Tas5782Codec>(mock_i2c.GetProto(), std::move(unused_gpio0),
                                                       std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  {
    DaiFormat format = GetDefaultDaiFormat();
    format.frame_format = FRAME_FORMAT_STEREO_LEFT;  // This must fail, only I2S supported.
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, client.SetDaiFormat(std::move(format)));
  }
  {
    DaiFormat format = GetDefaultDaiFormat();
    format.number_of_channels = 1;  // Almost good format (wrong channels).
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, client.SetDaiFormat(std::move(format)));
  }

  mock_i2c.VerifyAndClear();
}

TEST(Tas5782Test, GetDai) {
  mock_i2c::MockI2c mock_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;

  auto codec = SimpleCodecServer::Create<Tas5782Codec>(mock_i2c.GetProto(), std::move(unused_gpio0),
                                                       std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);
  auto formats = client.GetDaiFormats();

  EXPECT_EQ(formats.value().size(), 1);
  EXPECT_EQ(formats.value()[0].number_of_channels.size(), 1);
  EXPECT_EQ(formats.value()[0].number_of_channels[0], 2);
  EXPECT_EQ(formats.value()[0].sample_formats.size(), 1);
  EXPECT_EQ(formats.value()[0].sample_formats[0], SAMPLE_FORMAT_PCM_SIGNED);
  EXPECT_EQ(formats.value()[0].frame_formats.size(), 1);
  EXPECT_EQ(formats.value()[0].frame_formats[0], FRAME_FORMAT_I2S);
  EXPECT_EQ(formats.value()[0].frame_rates.size(), 1);
  EXPECT_EQ(formats.value()[0].frame_rates[0], 48000);
  EXPECT_EQ(formats.value()[0].bits_per_slot.size(), 1);
  EXPECT_EQ(formats.value()[0].bits_per_slot[0], 32);
  EXPECT_EQ(formats.value()[0].bits_per_sample.size(), 1);
  EXPECT_EQ(formats.value()[0].bits_per_sample[0], 32);
  mock_i2c.VerifyAndClear();
}

TEST(Tas5782Test, GetInfo) {
  mock_i2c::MockI2c unused_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
  auto codec = SimpleCodecServer::Create<Tas5782Codec>(
      unused_i2c.GetProto(), std::move(unused_gpio0), std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  auto info = client.GetInfo();
  EXPECT_EQ(info.value().unique_id.compare(""), 0);
  EXPECT_EQ(info.value().manufacturer.compare("Texas Instruments"), 0);
  EXPECT_EQ(info.value().product_name.compare("TAS5782m"), 0);
}

TEST(Tas5782Test, BridgedMode) {
  mock_i2c::MockI2c unused_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
  auto codec = SimpleCodecServer::Create<Tas5782Codec>(
      unused_i2c.GetProto(), std::move(unused_gpio0), std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  auto bridgeable = client.IsBridgeable();
  ASSERT_FALSE(bridgeable.value());
}

TEST(Tas5782Test, GetGainFormat) {
  mock_i2c::MockI2c unused_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
  auto codec = SimpleCodecServer::Create<Tas5782Codec>(
      unused_i2c.GetProto(), std::move(unused_gpio0), std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  auto format = client.GetGainFormat();
  EXPECT_EQ(format.value().min_gain_db, -103.0);
  EXPECT_EQ(format.value().max_gain_db, 24.0);
  EXPECT_EQ(format.value().gain_step_db, 0.5);
}

TEST(Tas5782Test, GetPlugState) {
  mock_i2c::MockI2c unused_i2c;
  ddk::GpioProtocolClient unused_gpio0, unused_gpio1;
  auto codec = SimpleCodecServer::Create<Tas5782Codec>(
      unused_i2c.GetProto(), std::move(unused_gpio0), std::move(unused_gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  auto state = client.GetPlugState();
  EXPECT_EQ(state.value().hardwired, true);
  EXPECT_EQ(state.value().plugged, true);
}

TEST(Tas5782Test, Init) {
  mock_i2c::MockI2c mock_i2c;
  mock_i2c
      .ExpectWriteStop({0x02, 0x10})   // Enter standby.
      .ExpectWriteStop({0x01, 0x11})   // Reset modules and registers.
      .ExpectWriteStop({0x0d, 0x10})   // The PLL reference clock is SCLK.
      .ExpectWriteStop({0x04, 0x01})   // PLL for MCLK setting.
      .ExpectWriteStop({0x28, 0x03})   // I2S, 32 bits.
      .ExpectWriteStop({0x2a, 0x22})   // Left DAC to Left ch, Right DAC to right channel.
      .ExpectWriteStop({0x02, 0x00});  // Exit standby.

  ddk::I2cChannel i2c(mock_i2c.GetProto());
  ddk::MockGpio mock_gpio0, mock_gpio1;
  ddk::GpioProtocolClient gpio0(mock_gpio0.GetProto());
  ddk::GpioProtocolClient gpio1(mock_gpio1.GetProto());
  mock_gpio0.ExpectWrite(ZX_OK, 0).ExpectWrite(ZX_OK, 1);  // Reset, set to 0 and then to 1.
  mock_gpio1.ExpectWrite(ZX_OK, 0).ExpectWrite(ZX_OK, 1);  // Set to mute and then to unmute..

  auto codec = SimpleCodecServer::Create<Tas5782Codec>(
    std::move(i2c), std::move(gpio0), std::move(gpio1));
  ASSERT_NOT_NULL(codec);
  auto codec_proto = codec->GetProto();
  SimpleCodecClient client;
  client.SetProtocol(&codec_proto);

  zx::nanosleep(zx::deadline_after(zx::msec(100)));
  client.Reset();
  mock_i2c.VerifyAndClear();
  mock_gpio0.VerifyAndClear();
  mock_gpio1.VerifyAndClear();
}
}  // namespace audio
