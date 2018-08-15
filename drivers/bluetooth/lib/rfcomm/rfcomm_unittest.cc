// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/drivers/bluetooth/lib/common/slab_allocator.h"
#include "garnet/drivers/bluetooth/lib/l2cap/fake_channel_test.h"
#include "garnet/drivers/bluetooth/lib/rfcomm/channel_manager.h"

namespace btlib {
namespace rfcomm {
namespace {

constexpr l2cap::ChannelId kL2CAPChannelId1 = 0x0040;

class RFCOMM_ChannelManagerTest : public l2cap::testing::FakeChannelTest {
 public:
  RFCOMM_ChannelManagerTest() : channel_manager_(nullptr) {}
  ~RFCOMM_ChannelManagerTest() override = default;

 protected:
  void SetUp() override {
    channel_manager_ = std::make_unique<ChannelManager>();
    FXL_DCHECK(channel_manager_);
  }

  void TearDown() override { channel_manager_.release(); }

  std::unique_ptr<ChannelManager> channel_manager_;
};

// Expect that registration of an L2CAP channel with the Channel Manager results
// in the L2CAP channel's eventual activation.
TEST_F(RFCOMM_ChannelManagerTest, RegisterL2CAPChannel) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));
  EXPECT_TRUE(l2cap_channel->activated());
}

// Test that command timeouts during multiplexer startup result in the session
// being closed down.
TEST_F(RFCOMM_ChannelManagerTest, MuxStartupAndParamNegotiation_Timeout) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::unique_ptr<const common::ByteBuffer> received_buffer;
  l2cap_channel->SetSendCallback(
      [&received_buffer](auto buffer) { received_buffer = std::move(buffer); },
      dispatcher());

  bool channel_received = false;
  fbl::RefPtr<Channel> channel;
  channel_manager_->OpenRemoteChannel(
      l2cap_channel->link_handle(), kMinServerChannel,
      [&channel, &channel_received](auto ch, auto server_channel) {
        channel_received = true;
        channel = ch;
      },
      dispatcher());
  RunLoopUntilIdle();

  {
    // Expect a SABM frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }

  // Do nothing
  RunLoopFor(zx::min(5));

  // Expect closedown after timeout
  EXPECT_FALSE(l2cap_channel->activated());
}

// Test successful multiplexer startup (resulting role: responder).
TEST_F(RFCOMM_ChannelManagerTest, MuxStartupAndParamNegotiation_Responder) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::unique_ptr<const common::ByteBuffer> received_buffer;
  l2cap_channel->SetSendCallback(
      [&received_buffer](auto buffer) { received_buffer = std::move(buffer); },
      dispatcher());

  {
    // Receive a multiplexer startup frame on the session
    auto frame = std::make_unique<SetAsynchronousBalancedModeCommand>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    // Expect a UA frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    EXPECT_EQ(FrameType::kUnnumberedAcknowledgement,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
}

// Test successful multiplexer startup (resulting role: initiator)
TEST_F(RFCOMM_ChannelManagerTest, MuxStartupAndParamNegotiation_Initiator) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::unique_ptr<const common::ByteBuffer> received_buffer;
  l2cap_channel->SetSendCallback(
      [&received_buffer](auto buffer) { received_buffer = std::move(buffer); },
      dispatcher());

  bool channel_received = false;
  fbl::RefPtr<Channel> channel;
  channel_manager_->OpenRemoteChannel(
      l2cap_channel->link_handle(), kMinServerChannel,
      [&channel, &channel_received](auto ch, auto server_channel) {
        channel_received = true;
        channel = ch;
      },
      dispatcher());
  RunLoopUntilIdle();

  {
    // Expect a SABM frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Receive a UA on the session
    auto frame = std::make_unique<UnnumberedAcknowledgementResponse>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    // Expect a PN command from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kInitiator, received_buffer->view());
    EXPECT_EQ(FrameType::kUnnumberedInfoHeaderCheck,
              static_cast<FrameType>(frame->control()));
    DLCI dlci = ServerChannelToDLCI(kMinServerChannel, Role::kInitiator);
    auto mux_command =
        static_cast<MuxCommandFrame*>(frame.get())->TakeMuxCommand();
    EXPECT_EQ(MuxCommandType::kDLCParameterNegotiation,
              mux_command->command_type());

    auto params =
        static_cast<DLCParameterNegotiationCommand*>(mux_command.get())
            ->params();
    EXPECT_EQ(dlci, params.dlci);
    params.credit_based_flow_handshake =
        CreditBasedFlowHandshake::kSupportedResponse;

    // Receive PN response
    frame = std::make_unique<MuxCommandFrame>(
        Role::kResponder, true,
        std::make_unique<DLCParameterNegotiationCommand>(
            CommandResponse::kResponse, params));
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }

  EXPECT_TRUE(channel_received);
  EXPECT_FALSE(channel);
}

// Test multiplexer startup conflict procedure (resulting role: initiator).
TEST_F(RFCOMM_ChannelManagerTest,
       MuxStartupAndParamNegotiation_Conflict_BecomeInitiator) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::unique_ptr<const common::ByteBuffer> received_buffer;
  l2cap_channel->SetSendCallback(
      [&received_buffer](auto buffer) { received_buffer = std::move(buffer); },
      dispatcher());

  bool channel_received = false;
  fbl::RefPtr<Channel> channel;
  channel_manager_->OpenRemoteChannel(
      l2cap_channel->link_handle(), kMinServerChannel,
      [&channel, &channel_received](auto ch, auto server_channel) {
        channel_received = true;
        channel = ch;
      },
      dispatcher());
  RunLoopUntilIdle();

  {
    // Expect a SABM frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Receive a conflicting SABM on the session
    auto frame = std::make_unique<SetAsynchronousBalancedModeCommand>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    // Expect a DM frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kDisconnectedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Wait and expect a SABM
    RunLoopFor(zx::sec(5));
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Receive a UA on the session
    auto frame = std::make_unique<UnnumberedAcknowledgementResponse>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    // Expect a PN command from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kInitiator, received_buffer->view());
    EXPECT_EQ(FrameType::kUnnumberedInfoHeaderCheck,
              static_cast<FrameType>(frame->control()));
    DLCI dlci = ServerChannelToDLCI(kMinServerChannel, Role::kInitiator);
    auto mux_command =
        static_cast<MuxCommandFrame*>(frame.get())->TakeMuxCommand();
    EXPECT_EQ(MuxCommandType::kDLCParameterNegotiation,
              mux_command->command_type());

    auto params =
        static_cast<DLCParameterNegotiationCommand*>(mux_command.get())
            ->params();
    EXPECT_EQ(dlci, params.dlci);
    params.credit_based_flow_handshake =
        CreditBasedFlowHandshake::kSupportedResponse;

    // Receive PN response
    frame = std::make_unique<MuxCommandFrame>(
        Role::kResponder, true,
        std::make_unique<DLCParameterNegotiationCommand>(
            CommandResponse::kResponse, params));
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }

  EXPECT_TRUE(channel_received);
  EXPECT_FALSE(channel);
}

// Test multiplexer startup conflict procedure (resulting role: responder).
TEST_F(RFCOMM_ChannelManagerTest,
       MuxStartupAndParamNegotiation_Conflict_BecomeResponder) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::queue<std::unique_ptr<const common::ByteBuffer>> received_buffers;
  l2cap_channel->SetSendCallback(
      [&received_buffers](auto buffer) {
        received_buffers.push(std::move(buffer));
      },
      dispatcher());

  bool channel_received = false;
  fbl::RefPtr<Channel> channel;
  channel_manager_->OpenRemoteChannel(
      l2cap_channel->link_handle(), kMinServerChannel,
      [&channel, &channel_received](auto ch, auto server_channel) {
        channel_received = true;
        channel = ch;
      },
      dispatcher());
  RunLoopUntilIdle();

  {
    // Expect a SABM frame from the session
    EXPECT_EQ(1ul, received_buffers.size());
    auto frame =
        Frame::Parse(true, Role::kUnassigned, received_buffers.front()->view());
    received_buffers.pop();
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Receive a conflicting SABM on the session
    auto frame = std::make_unique<SetAsynchronousBalancedModeCommand>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    // Expect a DM frame from the session
    EXPECT_EQ(1ul, received_buffers.size());
    auto frame =
        Frame::Parse(true, Role::kUnassigned, received_buffers.front()->view());
    received_buffers.pop();
    EXPECT_EQ(FrameType::kDisconnectedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Immediately receive another SABM on the session
    auto frame = std::make_unique<SetAsynchronousBalancedModeCommand>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    EXPECT_EQ(2ul, received_buffers.size());
    auto frame =
        Frame::Parse(true, Role::kUnassigned, received_buffers.front()->view());
    received_buffers.pop();
    EXPECT_EQ(FrameType::kUnnumberedAcknowledgement,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Expect a PN command from the session
    EXPECT_EQ(1ul, received_buffers.size());
    auto frame =
        Frame::Parse(true, Role::kResponder, received_buffers.front()->view());
    received_buffers.pop();
    EXPECT_EQ(FrameType::kUnnumberedInfoHeaderCheck,
              static_cast<FrameType>(frame->control()));
    DLCI dlci = ServerChannelToDLCI(kMinServerChannel, Role::kResponder);
    auto mux_command =
        static_cast<MuxCommandFrame*>(frame.get())->TakeMuxCommand();
    EXPECT_EQ(MuxCommandType::kDLCParameterNegotiation,
              mux_command->command_type());

    auto params =
        static_cast<DLCParameterNegotiationCommand*>(mux_command.get())
            ->params();
    EXPECT_EQ(dlci, params.dlci);
    params.credit_based_flow_handshake =
        CreditBasedFlowHandshake::kSupportedResponse;

    // Receive PN response
    frame = std::make_unique<MuxCommandFrame>(
        Role::kInitiator, true,
        std::make_unique<DLCParameterNegotiationCommand>(
            CommandResponse::kResponse, params));
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }

  EXPECT_TRUE(channel_received);
  EXPECT_FALSE(channel);
}

// Tests whether sessions handle invalid max frame sizes correctly.
TEST_F(RFCOMM_ChannelManagerTest,
       MuxStartupAndParamNegotiation_BadPN_InvalidMaxFrameSize) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::unique_ptr<const common::ByteBuffer> received_buffer;
  l2cap_channel->SetSendCallback(
      [&received_buffer](auto buffer) { received_buffer = std::move(buffer); },
      dispatcher());

  channel_manager_->OpenRemoteChannel(l2cap_channel->link_handle(),
                                      kMinServerChannel, {}, dispatcher());
  RunLoopUntilIdle();

  {
    // Expect a SABM frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Receive a UA on the session
    auto frame = std::make_unique<UnnumberedAcknowledgementResponse>(
        Role::kUnassigned, kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }

  DLCI dlci = ServerChannelToDLCI(kMinServerChannel, Role::kInitiator);

  {
    // Expect a PN command from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kInitiator, received_buffer->view());
    EXPECT_EQ(FrameType::kUnnumberedInfoHeaderCheck,
              static_cast<FrameType>(frame->control()));
    auto mux_command =
        static_cast<MuxCommandFrame*>(frame.get())->TakeMuxCommand();
    EXPECT_EQ(MuxCommandType::kDLCParameterNegotiation,
              mux_command->command_type());

    // Create invalid parameters.
    auto params =
        static_cast<DLCParameterNegotiationCommand*>(mux_command.get())
            ->params();
    EXPECT_EQ(dlci, params.dlci);
    params.credit_based_flow_handshake =
        CreditBasedFlowHandshake::kSupportedResponse;
    // Request a larger max frame size than what was proposed.
    params.maximum_frame_size += 1;

    // Receive PN response
    frame = std::make_unique<MuxCommandFrame>(
        Role::kResponder, true,
        std::make_unique<DLCParameterNegotiationCommand>(
            CommandResponse::kResponse, params));
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
  {
    // Expect a DISC
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kInitiator, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kDisconnect, static_cast<FrameType>(frame->control()));
    EXPECT_EQ(CommandResponse::kCommand, frame->command_response());
    EXPECT_EQ(dlci, frame->dlci());
  }
}

// A DM response to a mux SABM shouldn't crash (but shouldn't do anything else).
TEST_F(RFCOMM_ChannelManagerTest, MuxStartupAndParamNegotiation_DM) {
  ChannelOptions l2cap_channel_options(kL2CAPChannelId1);
  auto l2cap_channel = CreateFakeChannel(l2cap_channel_options);
  EXPECT_TRUE(channel_manager_->RegisterL2CAPChannel(l2cap_channel));

  // Get frames sent from our rfcomm::Session along this l2cap::Channel
  std::unique_ptr<const common::ByteBuffer> received_buffer;
  l2cap_channel->SetSendCallback(
      [&received_buffer](auto buffer) { received_buffer = std::move(buffer); },
      dispatcher());

  channel_manager_->OpenRemoteChannel(l2cap_channel->link_handle(),
                                      kMinServerChannel, {}, dispatcher());
  RunLoopUntilIdle();

  {
    // Expect a SABM frame from the session
    EXPECT_TRUE(received_buffer);
    auto frame = Frame::Parse(true, Role::kUnassigned, received_buffer->view());
    received_buffer = nullptr;
    EXPECT_EQ(FrameType::kSetAsynchronousBalancedMode,
              static_cast<FrameType>(frame->control()));
    EXPECT_EQ(kMuxControlDLCI, frame->dlci());
  }
  {
    // Receive a DM on the session
    auto frame = std::make_unique<DisconnectedModeResponse>(Role::kUnassigned,
                                                            kMuxControlDLCI);
    auto buffer = common::NewSlabBuffer(frame->written_size());
    frame->Write(buffer->mutable_view());
    l2cap_channel->Receive(buffer->view());
    RunLoopUntilIdle();
  }
}

}  // namespace
}  // namespace rfcomm
}  // namespace btlib
