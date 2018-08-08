// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_BLUETOOTH_LIB_RFCOMM_RFCOMM_H_
#define GARNET_DRIVERS_BLUETOOTH_LIB_RFCOMM_RFCOMM_H_

#include <cstdint>

#include <lib/fxl/logging.h>

#include "garnet/drivers/bluetooth/lib/common/byte_buffer.h"

namespace btlib {
namespace rfcomm {

// C/R bit, used at both the frame level and the multiplexer channel command
// level. See RFCOMM 5.1.3 and 5.4.6.1/2.
enum class CommandResponse { kCommand, kResponse };

// Role assigned to this device's end of the RFCOMM session. Start-up procedure
// is described in RFCOMM 5.2.1; the device which starts up the multiplexer
// control channel is considered the initiator (see "RFCOMM initiator" in the
// glossary, RFCOMM 9).
// A value of kUnassigned indicates that the RFCOMM session has not completed
// its start-up procedure, and thus no role has yet been assigned.
enum class Role { kUnassigned, kInitiator, kResponder };

// Return the Role opposite to the one given in |role|. The opposite of the
// Unassigned role is Unassigned. This is used to get our peer's role when we
// know our own.
inline constexpr Role OppositeRole(Role role) {
  return role == Role::kUnassigned
             ? Role::kUnassigned
             : role == Role::kInitiator ? Role::kResponder : Role::kInitiator;
}

// DLCIs are 6 bits. See RFCOMM 5.4. Any DLCI value will be truncated to the
// least significant 6 bits.
using DLCI = uint8_t;
// DLCI 0 is internally used by RFCOMM as the multiplexer control channel, over
// which the two multiplexers communicate. DLCIs 2-61 correspond to user data
// channels, which can be used by applications.
constexpr DLCI kMuxControlDLCI = 0;
constexpr DLCI kMinUserDLCI = 2;
constexpr DLCI kMaxUserDLCI = 61;

// Server Channels are 5 bits wide; they are the 5 most significant bits of the
// DLCI. Server Channels are exposed to the outside world; a user who is
// requesting to open a channel will know the Server Channel. DLCIs, on the
// other hand, are internal to RFCOMM.
using ServerChannel = uint8_t;
constexpr ServerChannel kMinServerChannel = 1;
constexpr ServerChannel kMaxServerChannel = 30;
// Used to indicate error.
constexpr ServerChannel kInvalidServerChannel = 0;

// Used to convert between Server Channel and DLCI. See RFCOMM 5.4 for the
// spec's description of Server Channels and how they relate to DLCIs.
constexpr size_t kServerChannelShift = 1;

inline constexpr ServerChannel DLCIToServerChannel(DLCI dlci) {
  FXL_DCHECK(dlci == kMuxControlDLCI ||
             (dlci >= kMinUserDLCI && dlci <= kMaxUserDLCI));
  return dlci >> kServerChannelShift;
}

inline constexpr DLCI ServerChannelToDLCI(ServerChannel server_channel,
                                          Role role) {
  FXL_DCHECK(role == Role::kInitiator || role == Role::kResponder);
  FXL_DCHECK(server_channel >= kMinServerChannel &&
             server_channel <= kMaxServerChannel);
  return (server_channel << kServerChannelShift) |
         (role == Role::kInitiator ? 1 : 0);
}

// The length field encodes the length of the information (payload) field. The
// length field can be one or two octets, and can encode at most a 15-bit value.
using InformationLength = uint16_t;

// The maximum Length value which can be encoded in a single-octet length field.
// This constant is used to quickly determine whether two (or more) length
// octets will be needed to encode a length value. It is used by Frames and
// MuxCommands alike.
constexpr InformationLength kMaxSingleOctetLength = 127;

// Encodes the Control Field; see table 2, GSM 07.10 5.2.1.3 and RFCOMM 4.2.
// The P/F bit is set to 0 for all frame types.
// clang-format off
enum class FrameType : uint8_t {
  kSetAsynchronousBalancedMode  = 0b00101111,
  kUnnumberedAcknowledgement    = 0b01100011,
  kDisconnectedMode             = 0b00001111,
  kDisconnect                   = 0b01000011,
  kUnnumberedInfoHeaderCheck    = 0b11101111
};
// clang-format on

}  // namespace rfcomm
}  // namespace btlib

#endif  // GARNET_DRIVERS_BLUETOOTH_LIB_RFCOMM_RFCOMM_H_
