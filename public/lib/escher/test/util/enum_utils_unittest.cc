// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/escher/util/enum_utils.h"

#include "gtest/gtest.h"

namespace {
using namespace escher;

enum class EnumForCycling { kZero = 0, kOne, kTwo, kThree, kEnumCount };

TEST(EnumCycle, NextAndPrevious) {
  EXPECT_EQ(EnumForCycling::kThree, EnumCycle(EnumForCycling::kTwo));
  EXPECT_EQ(EnumForCycling::kOne, EnumCycle(EnumForCycling::kTwo, true));
}

TEST(EnumCycle, Wraparound) {
  EXPECT_EQ(EnumForCycling::kZero, EnumCycle(EnumForCycling::kThree));
  EXPECT_EQ(EnumForCycling::kThree, EnumCycle(EnumForCycling::kZero, true));
}

}  // namespace
