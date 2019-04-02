// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/find_name.h"
#include "gtest/gtest.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "src/developer/debug/zxdb/expr/found_name.h"
#include "src/developer/debug/zxdb/expr/identifier.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/namespace.h"
#include "src/developer/debug/zxdb/symbols/process_symbols_test_setup.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"
#include "src/developer/debug/zxdb/symbols/type_test_support.h"
#include "src/developer/debug/zxdb/symbols/variable_test_support.h"
#include "src/lib/fxl/logging.h"

// NOTE: Finding variables on *this* and subclasses is
// SymbolEvalContextTest.FoundThis which tests both of our file's finding code
// as well as the decoding code.

namespace zxdb {

namespace {

ModuleSymbolIndexNode::RefType RefTypeForSymbol(
    const fxl::RefPtr<Symbol>& sym) {
  if (sym->AsType())
    return ModuleSymbolIndexNode::RefType::kType;
  if (sym->AsNamespace())
    return ModuleSymbolIndexNode::RefType::kNamespace;
  if (sym->AsFunction())
    return ModuleSymbolIndexNode::RefType::kFunction;
  if (sym->AsVariable())
    return ModuleSymbolIndexNode::RefType::kVariable;

  FXL_NOTREACHED();
  return ModuleSymbolIndexNode::RefType::kVariable;
}

// Creates a symbol in the index and the mock module symbols.
struct TestIndexedSymbol {
  // Index of the next DieRef to generated. This ensures the generated IDs are
  // unique.
  static int next_die_ref;

  TestIndexedSymbol(MockModuleSymbols* mod_sym,
                    ModuleSymbolIndexNode* index_parent,
                    const std::string& name, fxl::RefPtr<Symbol> sym)
      : die_ref(RefTypeForSymbol(sym), next_die_ref++),
        index_node(index_parent->AddChild(std::string(name))),
        symbol(std::move(sym)) {
    index_node->AddDie(die_ref);
    mod_sym->AddDieRef(die_ref, symbol);
  }

  // The DieRef links the index and the entry injected into the ModuleSymbols.
  ModuleSymbolIndexNode::DieRef die_ref;

  // Place where this variable is indexed.
  ModuleSymbolIndexNode* index_node;

  fxl::RefPtr<Symbol> symbol;
};

int TestIndexedSymbol::next_die_ref = 1;

// Creates a global variable that's inserted into the index and the mock
// ModuleSymbols.
struct TestGlobalVariable : public TestIndexedSymbol {
  TestGlobalVariable(MockModuleSymbols* mod_sym,
                     ModuleSymbolIndexNode* index_parent,
                     const std::string& var_name)
      : TestIndexedSymbol(mod_sym, index_parent, var_name,
                          MakeVariableForTest(var_name, MakeInt32Type(), 0x100,
                                              0x200, std::vector<uint8_t>())),
        var(symbol->AsVariable()) {}

  // The variable itself.
  fxl::RefPtr<Variable> var;
};

}  // namespace

// This test declares the following structure. There are three levels of
// variables, each one has one unique variable, and one labeled "value" for
// testing ambiguity.
//
// namespace ns {
//
// int32_t ns_value;
//
// void Foo(int32_t value, int32_t other_param) {
//   int32_t value;  // 2nd declaration.
//   int32_t function_local;
//   {
//     int32_t value;  // 3rd declaration.
//     int32_t block_local;
//   }
// }
//
// }  // namespace ns
TEST(FindName, FindLocalVariable) {
  ProcessSymbolsTestSetup setup;

  auto int32_type = MakeInt32Type();

  // Empty DWARF location expression. Since we don't evaluate any variables
  // they can all be empty.
  std::vector<uint8_t> var_loc;

  // Set up the module symbols. This creates "ns" and "ns_value" in the
  // symbol index.
  auto mod = std::make_unique<MockModuleSymbols>("mod.so");
  auto& root = mod->index().root();  // Root of the index for module 1.

  const char kNsName[] = "ns";
  auto ns_node = root.AddChild(kNsName);

  const char kNsVarName[] = "ns_value";
  TestGlobalVariable ns_value(mod.get(), ns_node, kNsVarName);

  constexpr uint64_t kLoadAddress = 0x1000;
  SymbolContext symbol_context(kLoadAddress);
  setup.InjectModule("mod", "1234", kLoadAddress, std::move(mod));

  // Namespace.
  auto ns = fxl::MakeRefCounted<Namespace>();
  ns->set_assigned_name(kNsName);

  // Function inside the namespace.
  auto function = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  function->set_assigned_name("function");
  uint64_t kFunctionBeginAddr = 0x1000;
  uint64_t kFunctionEndAddr = 0x2000;
  function->set_code_ranges(
      AddressRanges(AddressRange(kFunctionBeginAddr, kFunctionEndAddr)));
  function->set_parent(LazySymbol(ns));

  // Function parameters.
  auto param_value = MakeVariableForTest(
      "value", int32_type, kFunctionBeginAddr, kFunctionEndAddr, var_loc);
  auto param_other = MakeVariableForTest(
      "other_param", int32_type, kFunctionBeginAddr, kFunctionEndAddr, var_loc);
  function->set_parameters({LazySymbol(param_value), LazySymbol(param_other)});

  // Function local variables.
  auto var_value = MakeVariableForTest("value", int32_type, kFunctionBeginAddr,
                                       kFunctionEndAddr, var_loc);
  auto var_other =
      MakeVariableForTest("function_local", int32_type, kFunctionBeginAddr,
                          kFunctionEndAddr, var_loc);
  function->set_variables({LazySymbol(var_value), LazySymbol(var_other)});

  // Inner block.
  uint64_t kBlockBeginAddr = 0x1100;
  uint64_t kBlockEndAddr = 0x1200;
  auto block = fxl::MakeRefCounted<CodeBlock>(DwarfTag::kLexicalBlock);
  block->set_code_ranges(
      AddressRanges(AddressRange(kBlockBeginAddr, kBlockEndAddr)));
  block->set_parent(LazySymbol(function));
  function->set_inner_blocks({LazySymbol(block)});

  // Inner block variables.
  auto block_value = MakeVariableForTest("value", int32_type, kBlockBeginAddr,
                                         kBlockEndAddr, var_loc);
  auto block_other = MakeVariableForTest(
      "block_local", int32_type, kBlockBeginAddr, kBlockEndAddr, var_loc);
  block->set_variables({LazySymbol(block_value), LazySymbol(block_other)});

  // Find "value" in the nested block should give the block's one.
  Identifier value_ident(
      ExprToken(ExprTokenType::kName, var_value->GetAssignedName(), 0));
  FoundName found =
      FindName(nullptr, block.get(), &symbol_context, value_ident);
  EXPECT_TRUE(found);
  EXPECT_EQ(block_value.get(), found.variable());

  // Find "value" in the function block should give the function's one.
  found = FindName(nullptr, function.get(), &symbol_context, value_ident);
  EXPECT_TRUE(found);
  EXPECT_EQ(var_value.get(), found.variable());

  // Find "::value" should match nothing.
  Identifier value_global_ident(Identifier::Component(
      ExprToken(ExprTokenType::kColonColon, "::", 0),
      ExprToken(ExprTokenType::kName, var_value->GetAssignedName(), 0)));
  found =
      FindName(nullptr, function.get(), &symbol_context, value_global_ident);
  EXPECT_FALSE(found);

  // Find "block_local" in the block should be found, but in the function it
  // should not be.
  Identifier block_local_ident(
      ExprToken(ExprTokenType::kName, block_other->GetAssignedName(), 0));
  found = FindName(nullptr, block.get(), &symbol_context, block_local_ident);
  EXPECT_TRUE(found);
  EXPECT_EQ(block_other.get(), found.variable());
  found = FindName(nullptr, function.get(), &symbol_context, block_local_ident);
  EXPECT_FALSE(found);

  // Finding the other function parameter in the block should work.
  Identifier other_param_ident(
      ExprToken(ExprTokenType::kName, param_other->GetAssignedName(), 0));
  found = FindName(nullptr, block.get(), &symbol_context, other_param_ident);
  EXPECT_TRUE(found);
  EXPECT_EQ(param_other.get(), found.variable());

  // Look up the variable "ns::ns_value" using the name "ns_value" (no
  // namespace) from within the context of the "ns::function()" function.
  // The namespace of the function should be implicitly picked up.
  Identifier ns_value_ident(ExprToken(ExprTokenType::kName, kNsVarName, 0));
  found =
      FindName(&setup.process(), block.get(), &symbol_context, ns_value_ident);
  EXPECT_TRUE(found);
  EXPECT_EQ(ns_value.var.get(), found.variable());

  // Loop up the global "ns_value" var with no global symbol context. This
  // should fail and not crash.
  found = FindName(nullptr, block.get(), &symbol_context, ns_value_ident);
  EXPECT_FALSE(found);

  // Break reference cycle for test teardown.
  function->set_parent(LazySymbol());
  block->set_parent(LazySymbol());
}

// This only tests the ModuleSymbols and function naming integration, the
// details of the index searching are tested by FindGlobalNameInModule()
TEST(FindName, FindGlobalName) {
  ProcessSymbolsTestSetup setup;

  const char kGlobalName[] = "global";  // Different variable in each.
  const char kVar1Name[] = "var1";      // Only in module 1
  const char kVar2Name[] = "var2";      // Only in module 2
  const char kNotFoundName[] = "notfound";

  Identifier global_ident(ExprToken(ExprTokenType::kName, kGlobalName, 0));
  Identifier var1_ident(ExprToken(ExprTokenType::kName, kVar1Name, 0));
  Identifier var2_ident(ExprToken(ExprTokenType::kName, kVar2Name, 0));
  Identifier notfound_ident(ExprToken(ExprTokenType::kName, kNotFoundName, 0));

  // Module 1.
  auto mod1 = std::make_unique<MockModuleSymbols>("mod1.so");
  auto& root1 = mod1->index().root();  // Root of the index for module 1.
  TestGlobalVariable global1(mod1.get(), &root1, kGlobalName);
  TestGlobalVariable var1(mod1.get(), &root1, kVar1Name);
  constexpr uint64_t kLoadAddress1 = 0x1000;
  SymbolContext symbol_context1(kLoadAddress1);
  setup.InjectModule("mod1", "1234", kLoadAddress1, std::move(mod1));

  // Module 2.
  auto mod2 = std::make_unique<MockModuleSymbols>("mod2.so");
  auto& root2 = mod2->index().root();  // Root of the index for module 1.
  TestGlobalVariable global2(mod2.get(), &root2, kGlobalName);
  TestGlobalVariable var2(mod2.get(), &root2, kVar2Name);
  constexpr uint64_t kLoadAddress2 = 0x2000;
  SymbolContext symbol_context2(kLoadAddress2);
  setup.InjectModule("mod2", "5678", kLoadAddress2, std::move(mod2));

  // Searching for "global" in module1's context should give the global in that
  // module.
  auto found = FindGlobalName(&setup.process(), Identifier(), &symbol_context1,
                              global_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(global1.var.get(), found.variable());

  // Searching for "global" in module2's context should give the global in that
  // module.
  found = FindGlobalName(&setup.process(), Identifier(), &symbol_context2,
                         global_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(global2.var.get(), found.variable());

  // Searching for "var1" in module2's context should still find it even though
  // its in the other module.
  found = FindGlobalName(&setup.process(), Identifier(), &symbol_context2,
                         var1_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(var1.var.get(), found.variable());

  // Searching for "var2" with no context should still find it.
  found = FindGlobalName(&setup.process(), Identifier(), nullptr, var2_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(var2.var.get(), found.variable());
}

TEST(FindName, FindGlobalNameInModule) {
  MockModuleSymbols mod_sym("test.so");

  auto& root = mod_sym.index().root();  // Root of the index.

  const char kVarName[] = "var";
  const char kNsName[] = "ns";

  // Make a global variable in the toplevel namespace.
  TestGlobalVariable global(&mod_sym, &root, kVarName);

  Identifier var_ident(ExprToken(ExprTokenType::kName, kVarName, 0));
  auto found = FindGlobalNameInModule(&mod_sym, Identifier(), var_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(global.var.get(), found.variable());

  // Say we're in some nested namespace and search for the same name. It should
  // find the variable in the upper namespace.
  Identifier nested_ns(ExprToken(ExprTokenType::kName, kNsName, 0));
  found = FindGlobalNameInModule(&mod_sym, nested_ns, var_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(global.var.get(), found.variable());

  // Add a variable in the nested namespace with the same name.
  auto ns_node = root.AddChild(kNsName);
  TestGlobalVariable ns(&mod_sym, ns_node, kVarName);

  // Re-search for the same name in the nested namespace, it should get the
  // nested one first.
  found = FindGlobalNameInModule(&mod_sym, nested_ns, var_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(ns.var.get(), found.variable());

  // Now do the same search but globally qualify the input "::var" which should
  // match only the toplevel one.
  Identifier var_global_ident(
      Identifier::Component(ExprToken(ExprTokenType::kColonColon, "::", 0),
                            ExprToken(ExprTokenType::kName, kVarName, 0)));
  found = FindGlobalNameInModule(&mod_sym, nested_ns, var_global_ident);
  ASSERT_TRUE(found);
  EXPECT_EQ(global.var.get(), found.variable());
}

TEST(FindName, FindTypeName) {
  ProcessSymbolsTestSetup setup;
  auto mod = std::make_unique<MockModuleSymbols>("mod.so");
  auto& root = mod->index().root();  // Root of the index for module 1.

  const char kGlobalTypeName[] = "GlobalType";
  const char kChildTypeName[] = "ChildType";  // "GlobalType::ChildType".

  // Global class name.
  Identifier global_type_name(
      ExprToken(ExprTokenType::kName, kGlobalTypeName, 0));
  auto global_type = fxl::MakeRefCounted<Collection>(DwarfTag::kClassType);
  global_type->set_assigned_name(kGlobalTypeName);
  TestIndexedSymbol global_indexed(mod.get(), &root, kGlobalTypeName,
                                   global_type);

  // Child type definition inside the global class name. Currently types don't
  // have child types and everything is found via the index.
  Identifier child_type_name(
      ExprToken(ExprTokenType::kName, kChildTypeName, 0));
  auto [err, full_child_type_name] =
      Identifier::FromString("GlobalType::ChildType");
  ASSERT_FALSE(err.has_error());
  auto child_type = fxl::MakeRefCounted<Collection>(DwarfTag::kClassType);
  child_type->set_assigned_name(kChildTypeName);
  TestIndexedSymbol child_indexed(mod.get(), global_indexed.index_node,
                                  kChildTypeName, child_type);

  // Declares a variable that points to the GlobalType. It will be the "this"
  // pointer for the function. The address range of this variable doesn't
  // overlap the function. This means we can never compute its value, but since
  // it's syntactically in-scope, we should still be able to use its type
  // to resolve type names on the current class.
  auto global_type_ptr = fxl::MakeRefCounted<ModifiedType>(
      DwarfTag::kPointerType, LazySymbol(global_type));
  auto this_var = MakeVariableForTest(
      "this", global_type_ptr, 0x9000, 0x9001,
      {llvm::dwarf::DW_OP_reg0, llvm::dwarf::DW_OP_stack_value});

  // Function as a member of GlobalType.
  auto function = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  function->set_assigned_name("function");
  uint64_t kFunctionBeginAddr = 0x1000;
  uint64_t kFunctionEndAddr = 0x2000;
  function->set_code_ranges(
      AddressRanges(AddressRange(kFunctionBeginAddr, kFunctionEndAddr)));
  function->set_object_pointer(LazySymbol(this_var));

  // Warning: this moves out the "mod" variable so all variable setup needs to
  // go before here.
  constexpr uint64_t kLoadAddress = 0x1000;
  SymbolContext symbol_context(kLoadAddress);
  setup.InjectModule("mod", "1234", kLoadAddress, std::move(mod));

  // Look up the global function.
  FoundName found = FindName(&setup.process(), function.get(), &symbol_context,
                             global_type_name);
  EXPECT_TRUE(found);
  EXPECT_EQ(FoundName::kType, found.kind());
  EXPECT_EQ(global_type.get(), found.type().get());

  // Look up the child function by full name.
  found = FindName(&setup.process(), function.get(), &symbol_context,
                   full_child_type_name);
  EXPECT_TRUE(found);
  EXPECT_EQ(FoundName::kType, found.kind());
  EXPECT_EQ(child_type.get(), found.type().get());

  // Look up the child function by just the child name. Since the function is
  // a member of GlobalType, ChildType is a member of "this" so it should be
  // found.
  found = FindName(&setup.process(), function.get(), &symbol_context,
                   child_type_name);
  EXPECT_TRUE(found);
  EXPECT_EQ(FoundName::kType, found.kind());
  EXPECT_EQ(child_type.get(), found.type().get());
}

TEST(FindName, FindTemplateName) {
  ProcessSymbolsTestSetup setup;
  auto mod = std::make_unique<MockModuleSymbols>("mod.so");
  auto& root = mod->index().root();  // Root of the index for module 1.

  // Declare two functions, one's a template, the other has the same prefix but
  // isn't.
  const char kTemplateIntName[] = "Template<int>";
  const char kTemplateNotName[] = "TemplateNot";

  Identifier template_int_name(
      ExprToken(ExprTokenType::kName, kTemplateIntName, 0));
  Identifier template_not_name(
      ExprToken(ExprTokenType::kName, kTemplateNotName, 0));

  auto template_int = fxl::MakeRefCounted<Collection>(DwarfTag::kClassType);
  template_int->set_assigned_name(kTemplateIntName);
  TestIndexedSymbol template_int_indexed(mod.get(), &root, kTemplateIntName,
                                         template_int);

  auto template_not = fxl::MakeRefCounted<Collection>(DwarfTag::kClassType);
  template_not->set_assigned_name(kTemplateNotName);
  TestIndexedSymbol template_not_indexed(mod.get(), &root, kTemplateNotName,
                                         template_not);

  // Warning: this moves out the "mod" variable so all variable setup needs to
  // go before here.
  constexpr uint64_t kLoadAddress = 0x1000;
  SymbolContext symbol_context(kLoadAddress);
  setup.InjectModule("mod", "1234", kLoadAddress, std::move(mod));

  // The string "Template" should be identified as one.
  Identifier template_name(ExprToken(ExprTokenType::kName, "Template", 0));
  auto found = FindName(&setup.process(), nullptr, nullptr, template_name);
  EXPECT_TRUE(found);
  EXPECT_EQ(FoundName::kTemplate, found.kind());

  // The string "TemplateNot" is a
  found = FindName(&setup.process(), nullptr, nullptr, template_not_name);
  EXPECT_TRUE(found);
  EXPECT_EQ(FoundName::kType, found.kind());
}

}  // namespace zxdb
