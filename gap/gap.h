// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdint>

// This file contains constants and numbers that are part of the Generic Access
// Profile specification.

namespace bluetooth {
namespace gap {

// EIR Data Type, Advertising Data Type (AD Type), OOB Data Type definitions.
enum class DataType : uint8_t {
  kFlags                        = 0x01,
  kIncomplete16BitServiceUUIDs  = 0x02,
  kComplete16BitServiceUUIDs    = 0x03,
  kIncomplete32BitServiceUUIDs  = 0x04,
  kComplete32BitServiceUUIDs    = 0x05,
  kIncomplete128BitServiceUUIDs = 0x06,
  kComplete128BitServiceUUIDs   = 0x07,
  kShortenedLocalName           = 0x08,
  kCompleteLocalName            = 0x09,
  kTXPowerLevel                 = 0x0A,
  kClassOfDevice                = 0x0D,
  kSSPOOBHash                   = 0x0E,
  kSSPOOBRandomizer             = 0x0F,

  // TODO(armansito): Complete this list.
};

// Potential values that can be provided in the "Flags" advertising data
// bitfield.
enum AdvFlag : uint8_t {
  // Octet 0
  kLELimitedDiscoverableMode        = (1 << 0),
  kLEGeneralDiscoverableMode        = (1 << 1),
  kBREDRNotSupported                = (1 << 2),
  kSimultaneousLEAndBREDRController = (1 << 3),
  kSimultaneousLEAndBREDRHost       = (1 << 4),
};

}  // namespace gap
}  // namespace bluetooth
