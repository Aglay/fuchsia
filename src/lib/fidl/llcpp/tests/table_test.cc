// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/llcpp/types/test/llcpp/fidl.h>
#include <gtest/gtest.h>

TEST(Table, UnownedBuilderBuildTablePrimitive) {
  namespace test = llcpp::fidl::llcpp::types::test;
  fidl::aligned<uint8_t> x = 3;
  fidl::aligned<uint8_t> y = 100;
  auto builder =
      test::SampleTable::UnownedBuilder().set_x(fidl::unowned_ptr(&x)).set_y(fidl::unowned_ptr(&y));
  const auto& table = builder.build();

  ASSERT_TRUE(table.has_x());
  ASSERT_TRUE(table.has_y());
  ASSERT_FALSE(table.has_vector_of_struct());
  ASSERT_EQ(table.x(), x);
  ASSERT_EQ(table.y(), y);
}

TEST(Table, BuilderBuildTablePrimitive) {
  namespace test = llcpp::fidl::llcpp::types::test;
  fidl::aligned<uint8_t> x = 3;
  fidl::aligned<uint8_t> y = 100;
  test::SampleTable::Frame frame;
  auto builder = test::SampleTable::Builder(fidl::unowned_ptr(&frame))
                     .set_x(fidl::unowned_ptr(&x))
                     .set_y(fidl::unowned_ptr(&y));
  const auto& table = builder.build();

  ASSERT_TRUE(table.has_x());
  ASSERT_TRUE(table.has_y());
  ASSERT_FALSE(table.has_vector_of_struct());
  ASSERT_EQ(table.x(), x);
  ASSERT_EQ(table.y(), y);
}

TEST(Table, UnownedBuilderBuildTableVectorOfStruct) {
  namespace test = llcpp::fidl::llcpp::types::test;
  std::vector<test::CopyableStruct> structs = {
      {.x = 30},
      {.x = 42},
  };
  fidl::VectorView<test::CopyableStruct> vector_view = fidl::unowned_vec(structs);
  auto builder =
      test::SampleTable::UnownedBuilder().set_vector_of_struct(fidl::unowned_ptr(&vector_view));
  const auto& table = builder.build();

  ASSERT_FALSE(table.has_x());
  ASSERT_FALSE(table.has_y());
  ASSERT_TRUE(table.has_vector_of_struct());
  ASSERT_EQ(table.vector_of_struct().count(), structs.size());
  ASSERT_EQ(table.vector_of_struct()[0].x, structs[0].x);
  ASSERT_EQ(table.vector_of_struct()[1].x, structs[1].x);
}

TEST(Table, BuilderBuildTableVectorOfStruct) {
  namespace test = llcpp::fidl::llcpp::types::test;
  std::vector<test::CopyableStruct> structs = {
      {.x = 30},
      {.x = 42},
  };
  fidl::VectorView<test::CopyableStruct> vector_view = fidl::unowned_vec(structs);
  test::SampleTable::Frame frame;
  auto builder = test::SampleTable::Builder(fidl::unowned_ptr(&frame))
                     .set_vector_of_struct(fidl::unowned_ptr(&vector_view));
  const auto& table = builder.build();

  ASSERT_FALSE(table.has_x());
  ASSERT_FALSE(table.has_y());
  ASSERT_TRUE(table.has_vector_of_struct());
  ASSERT_EQ(table.vector_of_struct().count(), structs.size());
  ASSERT_EQ(table.vector_of_struct()[0].x, structs[0].x);
  ASSERT_EQ(table.vector_of_struct()[1].x, structs[1].x);
}

TEST(Table, UnownedBuilderBuildEmptyTable) {
  namespace test = llcpp::fidl::llcpp::types::test;
  auto builder = test::SampleEmptyTable::UnownedBuilder();
  const auto& table = builder.build();
  ASSERT_TRUE(table.IsEmpty());
}

TEST(Table, BuilderBuildEmptyTable) {
  namespace test = llcpp::fidl::llcpp::types::test;
  fidl::aligned<test::SampleEmptyTable::Frame> frame;
  auto builder = test::SampleEmptyTable::Builder(fidl::unowned_ptr(&frame));
  const auto& table = builder.build();
  ASSERT_TRUE(table.IsEmpty());
}
