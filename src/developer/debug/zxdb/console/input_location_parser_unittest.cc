// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/input_location_parser.h"

#include "gtest/gtest.h"
#include "src/developer/debug/zxdb/client/mock_frame.h"
#include "src/developer/debug/zxdb/client/mock_process.h"
#include "src/developer/debug/zxdb/client/mock_target.h"
#include "src/developer/debug/zxdb/client/mock_thread.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/index_test_support.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/namespace.h"
#include "src/developer/debug/zxdb/symbols/process_symbols_test_setup.h"

namespace zxdb {

namespace {

class InputLocationParserTest : public testing::Test {
 public:
  void SetUp() override { mock_module_symbols_ = symbols_.InjectMockModule(); }

 protected:
  ProcessSymbolsTestSetup symbols_;
  MockModuleSymbols* mock_module_symbols_ = nullptr;
  SymbolContext symbol_context_ = SymbolContext(ProcessSymbolsTestSetup::kDefaultLoadAddress);
};

}  // namespace

TEST_F(InputLocationParserTest, ParseGlobal) {
  InputLocation location;

  SymbolContext relative_context = SymbolContext::ForRelativeAddresses();

  // Valid symbol (including colons).
  Err err = ParseGlobalInputLocation(nullptr, "Foo::Bar", &location);
  EXPECT_FALSE(err.has_error());
  EXPECT_EQ(InputLocation::Type::kSymbol, location.type);
  EXPECT_EQ(R"("Foo"; ::"Bar")", location.symbol.GetDebugName());

  // Valid file/line.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "foo/bar.cc:123", &location);
  EXPECT_FALSE(err.has_error());
  EXPECT_EQ(InputLocation::Type::kLine, location.type);
  EXPECT_EQ("foo/bar.cc", location.line.file());
  EXPECT_EQ(123, location.line.line());

  // Invalid file/line.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "foo/bar.cc:123x", &location);
  EXPECT_TRUE(err.has_error());

  // Valid hex address with *.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "*0x12345f", &location);
  EXPECT_FALSE(err.has_error());
  EXPECT_EQ(InputLocation::Type::kAddress, location.type);
  EXPECT_EQ(0x12345fu, location.address);

  // Valid hex address without a *.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "0x12345f", &location);
  EXPECT_FALSE(err.has_error());
  EXPECT_EQ(InputLocation::Type::kAddress, location.type);
  EXPECT_EQ(0x12345fu, location.address);

  // Decimal number with "*" override should be an address.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "*21", &location);
  EXPECT_FALSE(err.has_error());
  EXPECT_EQ(InputLocation::Type::kAddress, location.type);
  EXPECT_EQ(21u, location.address);

  // Invalid address.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "*2134x", &location);
  EXPECT_TRUE(err.has_error());

  // Line number with no Frame for context.
  location = InputLocation();
  err = ParseGlobalInputLocation(nullptr, "21", &location);
  EXPECT_TRUE(err.has_error());

  // Implicit file name and valid frame but the location has no file name.
  MockFrame frame_no_file(nullptr, nullptr,
                          Location(0x1234, FileLine(), 0, relative_context, LazySymbol()),
                          0x12345678);
  location = InputLocation();
  err = ParseGlobalInputLocation(&frame_no_file, "21", &location);
  EXPECT_TRUE(err.has_error());

  // Valid implicit file name.
  std::string file = "foo.cc";
  MockFrame frame_valid(nullptr, nullptr,
                        Location(0x1234, FileLine(file, 12), 0, relative_context, LazySymbol()),
                        0x12345678);
  location = InputLocation();
  err = ParseGlobalInputLocation(&frame_valid, "21", &location);
  EXPECT_FALSE(err.has_error()) << err.msg();
  EXPECT_EQ(file, location.line.file());
  EXPECT_EQ(21, location.line.line());
}

TEST_F(InputLocationParserTest, ResolveInputLocation) {
  // Resolve to nothing.
  Location output;
  Err err = ResolveUniqueInputLocation(&symbols_.process(), nullptr, "Foo", false, &output);
  EXPECT_TRUE(err.has_error());
  EXPECT_EQ("Nothing matching this symbol was found.", err.msg());

  Location expected(0x12345678, FileLine("file.cc", 12), 0, symbol_context_);

  // Resolve to one location (success) case.
  mock_module_symbols_->AddSymbolLocations("Foo", {expected});
  err = ResolveUniqueInputLocation(&symbols_.process(), nullptr, "Foo", false, &output);
  EXPECT_FALSE(err.has_error());
  EXPECT_EQ(expected.address(), output.address());

  // Resolve to lots of locations, it should give suggestions. Even though we didn't request
  // symbols, the result should be symbolized.
  std::vector<Location> expected_locations;
  for (int i = 0; i < 15; i++) {
    // The address and line numbers count up for each match.
    expected_locations.emplace_back(0x12345000 + i, FileLine("file.cc", 100 + i), 0,
                                    symbol_context_);
  }
  mock_module_symbols_->AddSymbolLocations("Foo", expected_locations);

  // Resolve to all of them.
  std::vector<Location> output_locations;
  err = ResolveInputLocations(&symbols_.process(), nullptr, "Foo", false, &output_locations);
  EXPECT_FALSE(err.has_error());

  // The result should be the same as the input but not symbolized (we
  // requested no symbolization).
  ASSERT_EQ(expected_locations.size(), output_locations.size());
  for (size_t i = 0; i < expected_locations.size(); i++) {
    EXPECT_EQ(expected_locations[i].address(), output_locations[i].address());
    EXPECT_FALSE(output_locations[i].has_symbols());
  }

  // Try to resolve one of them. Since there are many this will fail. We requested no symbolization
  // but the error message should still be symbolized.
  err = ResolveUniqueInputLocation(&symbols_.process(), nullptr, "Foo", false, &output);
  EXPECT_TRUE(err.has_error());
  EXPECT_EQ(R"(This resolves to more than one location. Could be:
 • file.cc:100 = 0x12345000
 • file.cc:101 = 0x12345001
 • file.cc:102 = 0x12345002
 • file.cc:103 = 0x12345003
 • file.cc:104 = 0x12345004
 • file.cc:105 = 0x12345005
 • file.cc:106 = 0x12345006
 • file.cc:107 = 0x12345007
 • file.cc:108 = 0x12345008
 • file.cc:109 = 0x12345009
...5 more omitted...
)",
            err.msg());
}

// Tests that ParseLocalInputLocation() finds matches on the local object for symbolic names.
TEST_F(InputLocationParserTest, ParseLocalInputLocation) {
  auto& root = mock_module_symbols_->index().root();

  const char kFunctionName[] = "Foo";

  // The no-context case should just return the input symbol.
  std::vector<InputLocation> results;
  Err err = ParseLocalInputLocation(nullptr, kFunctionName, &results);
  ASSERT_TRUE(err.ok());
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(InputLocation::Type::kSymbol, results[0].type);
  EXPECT_EQ(R"("Foo")", results[0].symbol.GetDebugName());

  // Make a class.
  const char kClassName[] = "MyClass";
  auto my_class = fxl::MakeRefCounted<Collection>(DwarfTag::kClassType);
  my_class->set_assigned_name(kClassName);
  TestIndexedSymbol indexed_class(mock_module_symbols_, &root, kClassName, my_class);

  // Function inside the class.
  auto foo_func = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  foo_func->set_parent(my_class);
  foo_func->set_assigned_name(kFunctionName);
  constexpr uint64_t kFunctionBegin = ProcessSymbolsTestSetup::kDefaultLoadAddress + 0x1000;
  foo_func->set_code_ranges(AddressRanges(AddressRange(kFunctionBegin, kFunctionBegin + 0x10)));
  TestIndexedSymbol indexed_func(mock_module_symbols_, indexed_class.index_node, kFunctionName,
                                 foo_func);

  // Make a "this" pointer for the function pointing back to the class.
  auto my_class_ptr = fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, my_class);
  auto this_var =
      fxl::MakeRefCounted<Variable>(DwarfTag::kVariable, "this", my_class_ptr, VariableLocation());
  foo_func->set_object_pointer(this_var);

  // Process/thread setup.
  Session session;
  MockProcess process(&session);
  process.set_symbols(&symbols_.process());
  MockThread thread(&process);

  // The location points to the first address of the function.
  Location location(kFunctionBegin, FileLine(), 0, symbol_context_, foo_func);
  MockFrame frame(&session, &thread, location, 0x1000);

  // A new search should return the more specific version in the class, plus the global one.
  err = ParseLocalInputLocation(&frame, kFunctionName, &results);
  ASSERT_TRUE(err.ok());
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ(InputLocation::Type::kSymbol, results[0].type);
  EXPECT_EQ(R"("MyClass"; ::"Foo")", results[0].symbol.GetDebugName());
  EXPECT_EQ(InputLocation::Type::kSymbol, results[1].type);
  EXPECT_EQ(R"("Foo")", results[1].symbol.GetDebugName());

  // A fully qualified function name ("::Foo") should not match the current class and only the
  // global version should be returned.
  results.clear();
  err = ParseLocalInputLocation(&frame, std::string("::") + kFunctionName, &results);
  ASSERT_TRUE(err.ok());
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(InputLocation::Type::kSymbol, results[0].type);
  EXPECT_EQ(R"(::"Foo")", results[0].symbol.GetDebugName());
}

// Most of the prefix searching is tested by the FindName tests. This just tests the integration of
// that with the InputLocation completion.
TEST_F(InputLocationParserTest, CompleteInputLocation) {
  auto& root = mock_module_symbols_->index().root();

  // Global function.
  const char kGlobalName[] = "aGlobalFunction";
  auto global_func = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  global_func->set_assigned_name(kGlobalName);
  TestIndexedSymbol indexed_global(mock_module_symbols_, &root, kGlobalName, global_func);

  // Namespace
  const char kNsName[] = "aNamespace";
  auto ns = fxl::MakeRefCounted<Namespace>();
  ns->set_assigned_name(kNsName);
  TestIndexedSymbol indexed_ns(mock_module_symbols_, &root, kNsName, ns);

  // Class inside the namespace.
  const char kClassName[] = "Class";
  auto global_type = fxl::MakeRefCounted<Collection>(DwarfTag::kClassType);
  global_type->set_parent(ns);
  global_type->set_assigned_name(kClassName);
  TestIndexedSymbol indexed_type(mock_module_symbols_, indexed_ns.index_node, kClassName,
                                 global_type);

  // Function inside the class.
  const char kMemberName[] = "MemberFunction";
  auto member_func = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  member_func->set_assigned_name(kMemberName);
  member_func->set_parent(global_type);
  TestIndexedSymbol indexed_member(mock_module_symbols_, indexed_type.index_node, kMemberName,
                                   member_func);

  // TODO(brettw) make a test setup helper for a whole session / target / process / thread / frame +
  // symbols.
  Session session;
  MockTarget target(&session);
  target.set_symbols(&symbols_.target());
  MockProcess process(&session);
  process.set_symbols(&symbols_.process());

  Location loc;
  MockFrame frame(&session, nullptr, loc, 0);

  target.SetRunningProcess(&process);

  Command command;
  command.set_verb(Verb::kBreak);
  command.set_target(&target);
  command.set_frame(&frame);

  // TEST CODE -------------------------------------------------------------------------------------

  // "a" should complete to both "aNamespace" and "aGlobalFunction" (in that order).
  std::vector<std::string> found;
  CompleteInputLocation(command, "a", &found);
  ASSERT_EQ(2u, found.size());
  EXPECT_EQ("aNamespace::", found[0]);  // Namespaces get "::" appended.
  EXPECT_EQ(kGlobalName, found[1]);

  // "aNamespace::" doesn't complete to anything. It might be nice to have this complete to all
  // functions in the namespace, but we don't implement that yet. In the meantime, at least test
  // this does what we currently expect.
  found.clear();
  CompleteInputLocation(command, "aNamespace::", &found);
  ASSERT_TRUE(found.empty());

  // Completing classes.
  found.clear();
  CompleteInputLocation(command, "aNamespace::Cl", &found);
  ASSERT_EQ(1u, found.size());
  EXPECT_EQ("aNamespace::Class::", found[0]);  // Classes get "::" appended.

  // Completing class member functions.
  found.clear();
  CompleteInputLocation(command, "aNamespace::Class::M", &found);
  ASSERT_EQ(1u, found.size());
  EXPECT_EQ("aNamespace::Class::MemberFunction", found[0]);

  // Cleanup. Prevent reference cycles.
  member_func->set_parent(LazySymbol());
  global_type->set_parent(LazySymbol());
}

}  // namespace zxdb
