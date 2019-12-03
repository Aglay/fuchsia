// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/flat_ast.h>
#include <fidl/lexer.h>
#include <fidl/parser.h>
#include <fidl/source_file.h>
#include <unittest/unittest.h>

#include "test_library.h"

namespace {

static bool Compiles(const std::string& source_code,
                     std::vector<std::string>* out_errors = nullptr) {
  auto library = TestLibrary("test.fidl", source_code);
  const bool success = library.Compile();

  if (out_errors) {
    *out_errors = library.errors();
  }

  return success;
}

bool compiling() {
  BEGIN_TEST;

  std::vector<std::string> errors;

  // Populated fields.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    int64 i;
};
)FIDL"));

  // Reserved and populated fields.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    1: reserved;
    2: int64 x;
};
)FIDL"));

  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    1: int64 x;
    2: reserved;
};
)FIDL"));

  // Out of order fields.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    3: reserved;
    1: uint32 x;
    2: reserved;
};
)FIDL"));

  // Must have a non reserved field.
  EXPECT_FALSE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    1: reserved;
    2: reserved;
    3: reserved;
};
)FIDL",
                        &errors));

  EXPECT_EQ(errors.size(), 1u);
  ASSERT_STR_STR(errors.at(0).c_str(), "must have at least one non reserved member");

  // Duplicate ordinals.
  EXPECT_FALSE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    1: reserved;
    1: uint64 x;
};
)FIDL",
                        &errors));
  EXPECT_EQ(errors.size(), 1u);
  ASSERT_STR_STR(errors.at(0).c_str(), "Multiple xunion fields with the same ordinal");

  // Missing ordinals.
  EXPECT_FALSE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    1: uint32 x;
    3: reserved;
};
)FIDL",
                        &errors));
  EXPECT_EQ(errors.size(), 1u);
  ASSERT_STR_STR(errors.at(0).c_str(),
                 "missing ordinal 2 (ordinals must be dense); consider marking it reserved");

  // No zero ordinals.
  EXPECT_FALSE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    2: int32 y;
    0: int64 x;
};
)FIDL",
                        &errors));
  EXPECT_EQ(errors.size(), 1u);
  ASSERT_STR_STR(errors.at(0).c_str(), "ordinals must start at 1");

  // Explicit ordinals are valid.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    1: int64 x;
};
)FIDL"));

  // Cannot mix explicit/hashed ordinals
  EXPECT_FALSE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
    int32 y;
    1: int64 x;
};
)FIDL",
                        &errors));
  EXPECT_EQ(errors.size(), 1u);
  ASSERT_STR_STR(errors.at(0).c_str(), "cannot mix explicit and implicit ordinals");

  // Keywords as field names.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

struct struct {
    bool field;
};

xunion Foo {
    int64 xunion;
    bool library;
    uint32 uint32;
    struct member;
};
)FIDL"));

  // Recursion is allowed.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Value {
  bool bool_value;
  vector<Value?> list_value;
};
)FIDL"));

  // Mutual recursion is allowed.
  EXPECT_TRUE(Compiles(R"FIDL(
library fidl.test.xunions;

xunion Foo {
  Bar bar;
};

struct Bar {
  Foo? foo;
};
)FIDL"));

  END_TEST;
}

bool no_directly_recursive_xunions() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

xunion Value {
  Value value;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  auto errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_STR_STR(errors[0].c_str(), "There is an includes-cycle in declarations");

  END_TEST;
}

bool invalid_empty_xunions() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

xunion Foo {};

)FIDL");
  ASSERT_FALSE(library.Compile());
  auto errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_STR_STR(errors[0].c_str(), "must have at least one non reserved member");

  END_TEST;
}

bool union_xunion_same_ordinals_explicit() {
  BEGIN_TEST;

  TestLibrary xunion_library(R"FIDL(
library example;

xunion Foo {
  1: int8 bar;
};

)FIDL");
  ASSERT_TRUE(xunion_library.Compile());

  TestLibrary union_library(R"FIDL(
library example;

union Foo {
  1: int8 bar;
};

)FIDL");
  ASSERT_TRUE(union_library.Compile());

  const fidl::flat::XUnion* ex_xunion = xunion_library.LookupXUnion("Foo");
  const fidl::flat::Union* ex_union = union_library.LookupUnion("Foo");

  ASSERT_NOT_NULL(ex_xunion);
  ASSERT_NOT_NULL(ex_union);

  ASSERT_EQ(ex_union->members.front().xunion_ordinal->value, 1);
  ASSERT_EQ(ex_xunion->members.front().ordinal->value, 1);

  END_TEST;
}

bool error_syntax_explicit_ordinals() {
  BEGIN_TEST;
  TestLibrary error_library(R"FIDL(
library example;
protocol Example {
  Method() -> () error int32;
};
)FIDL");
  ASSERT_TRUE(error_library.Compile());
  const fidl::flat::Union* error_union = error_library.LookupUnion("Example_Method_Result");
  ASSERT_NOT_NULL(error_union);
  ASSERT_EQ(error_union->members.front().xunion_ordinal->value, 1);
  ASSERT_EQ(error_union->members.back().xunion_ordinal->value, 2);
  END_TEST;
}

bool no_nullable_members_in_xunions() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

xunion Foo {
  string? bar;
};

)FIDL");
  ASSERT_FALSE(library.Compile());
  auto errors = library.errors();
  ASSERT_EQ(errors.size(), 1);
  ASSERT_STR_STR(errors[0].c_str(), "Extensible union members cannot be nullable");

  END_TEST;
}

bool ordinal_cutoff() {
  BEGIN_TEST;

  TestLibrary below_cutoff(R"FIDL(
library example;

union Foo {
  512: string bar;
};

)FIDL");
  ASSERT_FALSE(below_cutoff.Compile());
  auto errors = below_cutoff.errors();
  ASSERT_EQ(errors.size(), 1);
  // the ordinal cutoff is enforced before checking for a dense ordinal space.
  ASSERT_STR_STR(errors[0].c_str(), "missing ordinal 1 (ordinals must be dense)");

  TestLibrary above_cutoff(R"FIDL(
library example;

union Foo {
  513: string bar;
};

)FIDL");
  ASSERT_FALSE(above_cutoff.Compile());
  errors = above_cutoff.errors();
  ASSERT_EQ(errors.size(), 1);
  // the ordinal cutoff is enforced before checking for a dense ordinal space.
  ASSERT_STR_STR(errors[0].c_str(), "explicit union ordinal must be <= 512");

  END_TEST;
}

}  // namespace

BEGIN_TEST_CASE(xunion_tests)
RUN_TEST(compiling)
RUN_TEST(no_directly_recursive_xunions)
RUN_TEST(invalid_empty_xunions)
RUN_TEST(union_xunion_same_ordinals_explicit)
RUN_TEST(error_syntax_explicit_ordinals)
RUN_TEST(no_nullable_members_in_xunions)
RUN_TEST(ordinal_cutoff)
END_TEST_CASE(xunion_tests)
