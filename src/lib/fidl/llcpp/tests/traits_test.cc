// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fidl/llcpp/traits.h"

#include <fidl/llcpp/types/test/llcpp/fidl.h>

#include "gtest/gtest.h"

namespace test = llcpp::fidl::llcpp::types::test;

// There's no actual code in here, but if it compiles, success.
TEST(Traits, NotConst) {
  static_assert(!fidl::IsStructOrTable<uint32_t>::value);
  static_assert(fidl::IsStructOrTable<test::CopyableStruct>::value);
  static_assert(fidl::IsStructOrTable<test::MoveOnlyStruct>::value);
  static_assert(fidl::IsStructOrTable<test::SampleTable>::value);

  static_assert(!fidl::IsTable<uint32_t>::value);
  static_assert(!fidl::IsTable<test::CopyableStruct>::value);
  static_assert(!fidl::IsTable<test::MoveOnlyStruct>::value);
  static_assert(fidl::IsTable<test::SampleTable>::value);

  static_assert(!fidl::IsStruct<uint32_t>::value);
  static_assert(fidl::IsStruct<test::CopyableStruct>::value);
  static_assert(fidl::IsStruct<test::MoveOnlyStruct>::value);
  static_assert(!fidl::IsStruct<test::SampleTable>::value);

  static_assert(!fidl::IsTableBuilder<uint32_t>::value);
  static_assert(!fidl::IsTableBuilder<test::CopyableStruct>::value);
  static_assert(!fidl::IsTableBuilder<test::MoveOnlyStruct>::value);
  static_assert(!fidl::IsTableBuilder<test::SampleTable>::value);
  static_assert(fidl::IsTableBuilder<test::SampleTable::Builder>::value);

  static_assert(!fidl::IsStringView<uint32_t>::value);
  static_assert(fidl::IsStringView<fidl::StringView>::value);

  static_assert(!fidl::IsVectorView<uint32_t>::value);
  static_assert(fidl::IsVectorView<fidl::VectorView<uint32_t>>::value);
}

TEST(Traits, Const) {
  static_assert(!fidl::IsStructOrTable<const uint32_t>::value);
  static_assert(fidl::IsStructOrTable<const test::CopyableStruct>::value);
  static_assert(fidl::IsStructOrTable<const test::MoveOnlyStruct>::value);
  static_assert(fidl::IsStructOrTable<const test::SampleTable>::value);

  static_assert(!fidl::IsTable<const uint32_t>::value);
  static_assert(!fidl::IsTable<const test::CopyableStruct>::value);
  static_assert(!fidl::IsTable<const test::MoveOnlyStruct>::value);
  static_assert(fidl::IsTable<const test::SampleTable>::value);

  static_assert(!fidl::IsStruct<const uint32_t>::value);
  static_assert(fidl::IsStruct<const test::CopyableStruct>::value);
  static_assert(fidl::IsStruct<const test::MoveOnlyStruct>::value);
  static_assert(!fidl::IsStruct<const test::SampleTable>::value);

  static_assert(!fidl::IsTableBuilder<const uint32_t>::value);
  static_assert(!fidl::IsTableBuilder<const test::CopyableStruct>::value);
  static_assert(!fidl::IsTableBuilder<const test::MoveOnlyStruct>::value);
  static_assert(!fidl::IsTableBuilder<const test::SampleTable>::value);
  static_assert(fidl::IsTableBuilder<const test::SampleTable::Builder>::value);

  static_assert(!fidl::IsStringView<const uint32_t>::value);
  static_assert(fidl::IsStringView<const fidl::StringView>::value);

  static_assert(!fidl::IsVectorView<const uint32_t>::value);
  static_assert(fidl::IsVectorView<const fidl::VectorView<uint32_t>>::value);
}
