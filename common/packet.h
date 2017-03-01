// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdint>

#include "lib/ftl/logging.h"

#include "apps/bluetooth/common/byte_buffer.h"

namespace bluetooth {
namespace common {

// Base class-template for generic packets that contain a header and a payload.
// A Packet is a light-weight object that operates over a previously allocated
// ByteBuffer without taking ownership of it.
//
// Example usage:
//
//   // Allocate a buffer
//   StaticByteBuffer<512> buffer;
//
//   // Receive some data on the buffer.
//   foo::WriteMyPacket(buffer.GetMutableData(), ...);
//
//   // Read packet header contents:
//   struct MyHeaderType {
//     uint8_t field0;
//   };
//
//   Packet<MyHeaderType> packet(&buffer, 0);
//   std::cout << "My header field is: " << packet.GetHeader().field0;
//
//   // If the packet has an expected payload size, pass that into the
//   // constructor:
//   struct MyPayloadType {
//     uint8_t byte_field;
//     uint16_t uint16_field;
//     uint8_t array_field[];
//   } __PACKED;
//
//   Packet<MyHeaderType> packet(&buffer, sizeof(MyPayloadType) + 2);
//   packet.GetPayload<MyPayloadType>().byte_field = 0xFF;
//   packet.GetPayload<MyPayloadType>().uint16_field = 0xFFFF;
//   packet.GetPayload<MyPayloadType>().array_field[0] = 0x00;
//   packet.GetPayload<MyPayloadType>().array_field[1] = 0x01;
//
// The Packet class does not expose a mutable getter for the header. Packet
// header contents are intended to be encoded by special subclasses that
// understand how to encode a particular packet type:
//
//   struct MyHeader {
//     uint8_t opcode;
//     uint16_t payload_size;
//   } __PACKED;
//
//   class MyPacket : Packet<MyHeader> {
//    public:
//     MyPacket(uint8_t opcode, ByteBuffer* buffer, size_t payload_size)
//         : Packet<MyHeader>(buffer, payload_size) {
//       // We encode the packet opcode here.
//       GetMutableHeader()->opcode = opcode;
//
//       // Our example protocol has an MTU of 65535 for the payload.
//       FTL_DCHECK(payload_size <= std::numeric_limits<uint16_t>::max());
//     }
//
//     // MyHeader contains a two-byte |payload_size| which needs to be properly
//     // encoded and decoded:
//
//     void EncodeHeader() override {
//       // Our example protocol expects the header fields to be encoded in
//       // little-endian and we don't want to assume host order:
//       size_t payload_size = GetPayloadSize();
//       GetMutableHeader()->payload_size =
//           LE16(static_cast<uint16_t>(GetPayloadSize()));
//     }
//
//     void DecodeHeader() override {
//       GetMutableHeader()->payload_size =
//           LE16(GetMutableHeader()->payload_size);
//     }
//   };
//
//   // Transmit data:
//   MyPacket packet(my_opcode, &buffer, sizeof(MyPayload));
//   packet.GetPayload<MyPayload>().stuff = foo;
//   packet.EncodeHeader();
//   foo::SendPacketOverTheWire(&packet);
//
//   // Receive data:
//   MyPacket packet(my_opcode, &received_bytes, sizeof(MyPayload));
//   packet.DecodeHeader();
//   rx_stuff = packet.GetPayload<MyPayload>().stuff;
template <typename HeaderType>
class Packet {
 public:
  // Initializes this Packet to operate over |buffer|. |payload_size| is the
  // size of the packet payload not including the packet header. A
  // |payload_size| value of 0 indicates that the packet contains no payload.
  explicit Packet(MutableByteBuffer* buffer, size_t payload_size = 0u)
      : buffer_(buffer), size_(sizeof(HeaderType) + payload_size) {
    FTL_DCHECK(buffer_);
    FTL_DCHECK(buffer_->GetSize() >= size_);
  }

  // Returns a reference to the beginning of the packet header. This may never
  // return nullptr.
  const HeaderType& GetHeader() const {
    return *reinterpret_cast<const HeaderType*>(buffer_->GetData());
  }

  // Returns a pointer to the beginning of the packet payload, immediately
  // following the header. Returns nullptr if the payload is empty.
  uint8_t* GetPayloadData() const {
    if (!GetPayloadSize()) return nullptr;
    return buffer_->GetMutableData() + sizeof(HeaderType);
  }

  // Returns the size of the packet payload, not including the header.
  size_t GetPayloadSize() const { return size_ - sizeof(HeaderType); }

  // Convenience getter that returns a pointer to the beginning of the packet
  // payload, immediately following the header, after casting it to a pointer of
  // the specified type. This is commonly used with packet protocol parameter
  // structures.
  template <typename PayloadType>
  PayloadType* GetPayload() const {
    FTL_DCHECK(sizeof(PayloadType) <= GetPayloadSize());
    return reinterpret_cast<PayloadType*>(GetPayloadData());
  }

  // Returns the packet size.
  size_t size() const { return size_; }

  // Returns a pointer to the underlying buffer.
  MutableByteBuffer* buffer() const { return buffer_; }

  // Encode and decode the header contents. A subclass implementation is
  // expected to finalize all header fields as required by the protocol and wire
  // format.
  virtual void EncodeHeader() {}
  virtual void DecodeHeader() {}

 protected:
  // Returns a pointer to the header that can be used to modify header contents.
  // Reserved for use by subclasses. We do this to encapsulate access to the
  // packet header contents.
  HeaderType* GetMutableHeader() const {
    return reinterpret_cast<HeaderType*>(buffer_->GetMutableData());
  }

 private:
  MutableByteBuffer* buffer_;  // weak
  size_t size_;
};

}  // namespace common
}  // namespace bluetooth
