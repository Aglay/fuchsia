// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <atomic>
 
#include <zxtest/zxtest.h>
#include <fbl/bitfield.h>

namespace {

const uint64_t test_val_3b = 0b101;
const uint64_t test_val_4b = 0b1001;
const uint64_t test_val_5b = 0b10001;
const uint64_t test_val_8b = 0b10000001;
const uint64_t test_val_13b = 0b1000000000001;

FBL_BITFIELD_DEF_START(TestBFuint64, uint64_t)
    FBL_BITFIELD_MEMBER(m0_3bits, 0, 3);
    FBL_BITFIELD_MEMBER(m1_4bits, 3, 4);
    FBL_BITFIELD_MEMBER(m2_8bits, 7, 8);
    FBL_BITFIELD_MEMBER(m3_13bits, 15, 13);
    FBL_BITFIELD_MEMBER(m4_5bits, 28, 5);
    FBL_BITFIELD_MEMBER(unused, 33, 31);
FBL_BITFIELD_DEF_END();

TEST(BitfieldTest, ReadWriteUint64) {
    TestBFuint64 bf;
    ASSERT_EQ(bf.value, 0u);

    ASSERT_EQ(bf.m0_3bits.maximum(), 7);
    ASSERT_EQ(bf.m1_4bits.maximum(), 15);
    ASSERT_EQ(bf.m2_8bits.maximum(), 255);
    ASSERT_EQ(bf.m3_13bits.maximum(), 8191);
    ASSERT_EQ(bf.m4_5bits.maximum(), 31);

    uint64_t test_val = 0;
    test_val |= test_val_3b << 0;
    test_val |= test_val_4b << 3;
    test_val |= test_val_8b << 7;
    test_val |= test_val_13b << 15;
    test_val |= test_val_5b << 28;

    bf.value = test_val;
    ASSERT_EQ(bf.m0_3bits, test_val_3b);
    ASSERT_EQ(bf.m1_4bits, test_val_4b);
    ASSERT_EQ(bf.m2_8bits, test_val_8b);
    ASSERT_EQ(bf.m3_13bits, test_val_13b);
    ASSERT_EQ(bf.m4_5bits, test_val_5b);
    ASSERT_EQ(bf.unused, 0u);

    bf.m0_3bits = 0u;
    ASSERT_EQ(bf.m0_3bits, 0u);
    ASSERT_EQ(bf.m1_4bits, test_val_4b);
    ASSERT_EQ(bf.m2_8bits, test_val_8b);
    ASSERT_EQ(bf.m3_13bits, test_val_13b);
    ASSERT_EQ(bf.m4_5bits, test_val_5b);
    ASSERT_EQ(bf.unused, 0u);

    bf.value = test_val;
    bf.m1_4bits = 0u;
    ASSERT_EQ(bf.m0_3bits, test_val_3b);
    ASSERT_EQ(bf.m1_4bits, 0u);
    ASSERT_EQ(bf.m2_8bits, test_val_8b);
    ASSERT_EQ(bf.m3_13bits, test_val_13b);
    ASSERT_EQ(bf.m4_5bits, test_val_5b);
    ASSERT_EQ(bf.unused, 0u);

    bf.value = test_val;
    bf.m2_8bits = 0u;
    ASSERT_EQ(bf.m0_3bits, test_val_3b);
    ASSERT_EQ(bf.m1_4bits, test_val_4b);
    ASSERT_EQ(bf.m2_8bits, 0u);
    ASSERT_EQ(bf.m3_13bits, test_val_13b);
    ASSERT_EQ(bf.m4_5bits, test_val_5b);
    ASSERT_EQ(bf.unused, 0u);

    bf.value = test_val;
    bf.m3_13bits = 0u;
    ASSERT_EQ(bf.m0_3bits, test_val_3b);
    ASSERT_EQ(bf.m1_4bits, test_val_4b);
    ASSERT_EQ(bf.m2_8bits, test_val_8b);
    ASSERT_EQ(bf.m3_13bits, 0u);
    ASSERT_EQ(bf.m4_5bits, test_val_5b);
    ASSERT_EQ(bf.unused, 0u);

    bf.value = test_val;
    bf.m4_5bits = 0u;
    ASSERT_EQ(bf.m0_3bits, test_val_3b);
    ASSERT_EQ(bf.m1_4bits, test_val_4b);
    ASSERT_EQ(bf.m2_8bits, test_val_8b);
    ASSERT_EQ(bf.m3_13bits, test_val_13b);
    ASSERT_EQ(bf.m4_5bits, 0u);
    ASSERT_EQ(bf.unused, 0u);
}

constexpr TestBFuint64 cex_bf_uint64;
static_assert(sizeof(cex_bf_uint64.m0_3bits) == sizeof(uint64_t));

}  // namespace
