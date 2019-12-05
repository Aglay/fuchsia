// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/packet_queue.h"

#include <lib/gtest/test_loop_fixture.h>

#include <fbl/ref_ptr.h>

#include "src/lib/syslog/cpp/logger.h"

namespace media::audio {
namespace {

class PacketQueueTest : public gtest::TestLoopFixture {
 protected:
  fbl::RefPtr<PacketQueue> CreatePacketQueue() {
    return fbl::MakeRefCounted<PacketQueue>(Format{{
        .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
        .channels = 2,
        .frames_per_second = 48000,
    }});
  }

  fbl::RefPtr<Packet> CreatePacket(uint32_t payload_buffer_id = 0) {
    auto vmo_mapper = fbl::MakeRefCounted<RefCountedVmoMapper>();
    zx_status_t res = vmo_mapper->CreateAndMap(PAGE_SIZE, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    if (res != ZX_OK) {
      FX_PLOGS(ERROR, res) << "Failed to map payload buffer";
      return nullptr;
    }
    return fbl::MakeRefCounted<Packet>(
        std::move(vmo_mapper), 0, FractionalFrames<uint32_t>(PAGE_SIZE),
        FractionalFrames<int64_t>(0), dispatcher(), [this] { ++released_packet_count_; });
  }

  size_t released_packet_count() const { return released_packet_count_; }

 private:
  size_t released_packet_count_ = 0;
};

TEST_F(PacketQueueTest, PushPacket) {
  auto packet_queue = CreatePacketQueue();

  // Enqueue a packet.
  ASSERT_TRUE(packet_queue->empty());

  packet_queue->PushPacket(CreatePacket());
  ASSERT_FALSE(packet_queue->empty());
  ASSERT_EQ(0u, released_packet_count());
}

TEST_F(PacketQueueTest, Flush) {
  auto packet_queue = CreatePacketQueue();

  // Enqueue a packet.
  ASSERT_TRUE(packet_queue->empty());
  packet_queue->PushPacket(CreatePacket());
  ASSERT_FALSE(packet_queue->empty());
  ASSERT_EQ(0u, released_packet_count());

  // Flush queue (discard all packets). Expect to see one packet released back to us.
  packet_queue->Flush(nullptr);
  RunLoopUntilIdle();

  ASSERT_TRUE(packet_queue->empty());
  ASSERT_EQ(1u, released_packet_count());
}

// Simulate the packet sink popping packets off the queue.
TEST_F(PacketQueueTest, LockUnlockPacket) {
  auto packet_queue = CreatePacketQueue();

  // Enqueue some packets.
  ASSERT_TRUE(packet_queue->empty());
  auto packet0 = CreatePacket(0);
  auto packet1 = CreatePacket(1);
  auto packet2 = CreatePacket(2);
  packet_queue->PushPacket(packet0);
  packet_queue->PushPacket(packet1);
  packet_queue->PushPacket(packet2);
  ASSERT_FALSE(packet_queue->empty());
  ASSERT_EQ(0u, released_packet_count());

  // Now pop off the packets in FIFO order.
  //
  // Packet #0:
  bool was_flushed = false;
  auto packet = packet_queue->LockPacket(&was_flushed);
  ASSERT_TRUE(was_flushed);
  ASSERT_NE(nullptr, packet);
  ASSERT_EQ(packet0.get(), packet.get());
  ASSERT_FALSE(packet_queue->empty());
  ASSERT_EQ(0u, released_packet_count());
  packet0 = nullptr;
  packet = nullptr;
  packet_queue->UnlockPacket(true);
  RunLoopUntilIdle();
  ASSERT_FALSE(packet_queue->empty());
  ASSERT_EQ(1u, released_packet_count());

  // Packet #1
  packet = packet_queue->LockPacket(&was_flushed);
  ASSERT_FALSE(was_flushed);
  ASSERT_NE(nullptr, packet);
  ASSERT_EQ(packet1.get(), packet.get());
  packet1 = nullptr;
  packet = nullptr;
  packet_queue->UnlockPacket(true);
  RunLoopUntilIdle();
  ASSERT_FALSE(packet_queue->empty());
  ASSERT_EQ(2u, released_packet_count());

  // ...and #2
  packet = packet_queue->LockPacket(&was_flushed);
  ASSERT_FALSE(was_flushed);
  ASSERT_NE(nullptr, packet);
  ASSERT_EQ(packet2.get(), packet.get());
  packet2 = nullptr;
  packet = nullptr;
  packet_queue->UnlockPacket(true);
  RunLoopUntilIdle();
  ASSERT_TRUE(packet_queue->empty());
  ASSERT_EQ(3u, released_packet_count());
}

}  // namespace
}  // namespace media::audio
