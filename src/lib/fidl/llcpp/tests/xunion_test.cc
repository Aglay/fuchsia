// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/event.h>
#include <lib/zx/eventpair.h>
#include <zircon/fidl.h>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <fidl/llcpp/types/test/llcpp/fidl.h>
#include <gtest/gtest.h>
#include <src/lib/fidl/llcpp/tests/test_utils.h>

namespace llcpp_test = ::llcpp::fidl::llcpp::types::test;

TEST(XUnionPayload, Primitive) {
  {
    llcpp_test::TestUnion test_union;
    EXPECT_TRUE(test_union.has_invalid_tag());
    int32_t primitive = 5;
    test_union.set_primitive(fidl::unowned_ptr(&primitive));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kPrimitive, test_union.which());
    EXPECT_EQ(5, test_union.primitive());
  }
  {
    int32_t primitive = 5;
    auto test_union = llcpp_test::TestUnion::WithPrimitive(fidl::unowned_ptr(&primitive));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kPrimitive, test_union.which());
    EXPECT_EQ(5, test_union.primitive());
  }
}

TEST(XUnionPayload, WhichDisallowedWhenUninitialized) {
  llcpp_test::TestUnion test_union;
  ASSERT_DEATH({ test_union.which(); }, "!has_invalid_tag()");
}

TEST(XUnionPayload, Struct) {
  llcpp_test::CopyableStruct copyable{.x = 5};
  auto test_xunion = llcpp_test::TestXUnion::WithCopyable(fidl::unowned_ptr(&copyable));
  EXPECT_EQ(llcpp_test::TestXUnion::Tag::kCopyable, test_xunion.which());
}

TEST(XUnionPayload, CopyableStruct) {
  {
    llcpp_test::TestUnion test_union;
    EXPECT_TRUE(test_union.has_invalid_tag());
    llcpp_test::CopyableStruct copyable_struct{.x = 5};
    test_union.set_copyable(fidl::unowned_ptr(&copyable_struct));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kCopyable, test_union.which());
  }
  {
    llcpp_test::CopyableStruct copyable_struct{.x = 5};
    auto test_union = llcpp_test::TestUnion::WithCopyable(fidl::unowned_ptr(&copyable_struct));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kCopyable, test_union.which());
  }
}

TEST(XUnionPayload, MoveOnlyStruct) {
  {
    llcpp_test::TestUnion test_union;
    EXPECT_TRUE(test_union.has_invalid_tag());
    llcpp_test::MoveOnlyStruct move_only_struct{.h = zx::handle()};
    test_union.set_move_only(fidl::unowned_ptr(&move_only_struct));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kMoveOnly, test_union.which());
  }
  {
    llcpp_test::TestUnion test_union;
    EXPECT_TRUE(test_union.has_invalid_tag());
    zx::event event;
    ASSERT_EQ(ZX_OK, zx::event::create(0, &event));
    llcpp_test::MoveOnlyStruct move_only_struct{.h = std::move(event)};
    EXPECT_NE(ZX_HANDLE_INVALID, move_only_struct.h.get());
    test_union.set_move_only(fidl::unowned_ptr(&move_only_struct));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kMoveOnly, test_union.which());
    EXPECT_NE(ZX_HANDLE_INVALID, move_only_struct.h.get());
  }
  {
    llcpp_test::MoveOnlyStruct move_only_struct{.h = zx::handle()};
    auto test_union = llcpp_test::TestUnion::WithMoveOnly(fidl::unowned_ptr(&move_only_struct));
    EXPECT_EQ(llcpp_test::TestUnion::Tag::kMoveOnly, test_union.which());
  }
}

bool IsPeerValid(const zx::unowned_eventpair& handle) {
  zx_signals_t observed_signals = {};
  switch (handle->wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::deadline_after(zx::msec(0)),
                           &observed_signals)) {
    case ZX_ERR_TIMED_OUT:
      // timeout implies peer-closed was not observed
      return true;
    case ZX_OK:
      return (observed_signals & ZX_EVENTPAIR_PEER_CLOSED) == 0;
    default:
      return false;
  }
}

TEST(MoveUnion, NoDoubleDestructPayload) {
  zx::eventpair canary_a, canary_b;
  ASSERT_EQ(zx::eventpair::create(0, &canary_a, &canary_b), ZX_OK);
  ASSERT_TRUE(IsPeerValid(zx::unowned_eventpair(canary_a)));
  // Initialize the union such that the handle |h| within the |MoveOnlyStruct|
  // payload overlaps with the eventpair.
  zx_handle_t h = canary_b.release();
  static_assert(sizeof(llcpp_test::TestUnion) == 24);
  uint8_t dangerous_buffer[sizeof(llcpp_test::TestUnion)] = {};
  memcpy(&dangerous_buffer[4], &h, sizeof(h));
  {
    llcpp_test::TestUnion* test_union = reinterpret_cast<llcpp_test::TestUnion*>(dangerous_buffer);
    llcpp_test::TestUnion union_with_absent_handle;
    llcpp_test::MoveOnlyStruct move_only_struct{.h = zx::handle()};
    union_with_absent_handle.set_move_only(fidl::unowned_ptr(&move_only_struct));
    // Manually running the move constructor.
    new (test_union) llcpp_test::TestUnion(std::move(union_with_absent_handle));
  }
  // |canary_b| should not be closed.
  EXPECT_TRUE(IsPeerValid(zx::unowned_eventpair(canary_a)));
  zx_handle_close(h);
}

TEST(XUnion, InitialTag) {
  llcpp_test::TestXUnion flexible_xunion;
  EXPECT_TRUE(flexible_xunion.has_invalid_tag());

  llcpp_test::TestStrictXUnion strict_xunion;
  EXPECT_TRUE(strict_xunion.has_invalid_tag());
}

TEST(XUnion, UnknownTagFlexible) {
  fidl_xunion_tag_t unknown_tag = 0x01020304;
  int32_t xunion_data = 0x0A0B0C0D;
  auto flexible_xunion = llcpp_test::TestXUnion::WithPrimitive(fidl::unowned_ptr(&xunion_data));

  // set to an unknown tag
  auto raw_bytes = reinterpret_cast<uint32_t*>(&flexible_xunion);
  raw_bytes[0] = unknown_tag;

  EXPECT_EQ(flexible_xunion.which(), llcpp_test::TestXUnion::Tag::kUnknown);
  int32_t unknown_data = *(static_cast<int32_t*>(flexible_xunion.unknownData()));
  EXPECT_EQ(unknown_data, xunion_data);
}

TEST(XUnion, UnknownTagStrict) {
  fidl_xunion_tag_t unknown_tag = 0x01020304;
  int32_t xunion_data = 0x0A0B0C0D;
  auto strict_xunion = llcpp_test::TestStrictXUnion::WithPrimitive(fidl::unowned_ptr(&xunion_data));

  // set to an unknown tag
  auto raw_bytes = reinterpret_cast<uint32_t*>(&strict_xunion);
  raw_bytes[0] = unknown_tag;
}
