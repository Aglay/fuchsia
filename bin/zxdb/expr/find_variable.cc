// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/expr/find_variable.h"

#include "garnet/bin/zxdb/expr/found_variable.h"
#include "garnet/bin/zxdb/expr/identifier.h"
#include "garnet/bin/zxdb/expr/index_walker.h"
#include "garnet/bin/zxdb/symbols/code_block.h"
#include "garnet/bin/zxdb/symbols/collection.h"
#include "garnet/bin/zxdb/symbols/data_member.h"
#include "garnet/bin/zxdb/symbols/function.h"
#include "garnet/bin/zxdb/symbols/loaded_module_symbols.h"
#include "garnet/bin/zxdb/symbols/module_symbol_index.h"
#include "garnet/bin/zxdb/symbols/module_symbol_index_node.h"
#include "garnet/bin/zxdb/symbols/module_symbols.h"
#include "garnet/bin/zxdb/symbols/process_symbols.h"
#include "garnet/bin/zxdb/symbols/type_utils.h"
#include "garnet/bin/zxdb/symbols/visit_scopes.h"

namespace zxdb {

namespace {

// Searches the given vector of values for one with the given name. If found,
// returns it, otherwise returns null.
const Variable* SearchVariableVector(const std::vector<LazySymbol>& vect,
                                     const std::string& search_for) {
  for (const auto& cur : vect) {
    const Variable* var = cur.Get()->AsVariable();
    if (!var)
      continue;  // Symbols are corrupt.
    if (search_for == var->GetAssignedName())
      return var;
  }
  return nullptr;
}

// Searches the list for a reference to a variable and returns the first one
// it finds.
std::optional<FoundVariable> GetVariableFromDieList(
    const ModuleSymbols* module_symbols,
    const std::vector<ModuleSymbolIndexNode::DieRef>& dies) {
  for (const ModuleSymbolIndexNode::DieRef& cur : dies) {
    LazySymbol lazy_symbol = module_symbols->IndexDieRefToSymbol(cur);
    if (!lazy_symbol)
      continue;
    const Symbol* symbol = lazy_symbol.Get();
    if (const Variable* var = symbol->AsVariable())
      return FoundVariable(var);
  }
  return std::nullopt;
}

}  // namespace

std::optional<FoundVariable> FindVariable(
    const ProcessSymbols* process_symbols, const CodeBlock* block,
    const SymbolContext* block_symbol_context, const Identifier& identifier) {
  if (block && !identifier.InGlobalNamespace()) {
    // Search for local variables and function parameters.
    if (auto found = FindLocalVariable(block, identifier))
      return found;

    // Search the "this" object.
    if (auto found = FindMemberOnThis(block, identifier))
      return found;
  }

  // Fall back to searching global vars.
  if (process_symbols) {
    return FindGlobalVariable(process_symbols, block, block_symbol_context,
                              identifier);
  }
  return std::nullopt;
}

std::optional<FoundVariable> FindLocalVariable(const CodeBlock* block,
                                               const Identifier& identifier) {
  // Local variables can only be simple names.
  const std::string* name = identifier.GetSingleComponentName();
  if (!name)
    return std::nullopt;

  // Search backwards in the nested lexical scopes searching for the first
  // variable or function parameter with the given name.
  const CodeBlock* cur_block = block;
  while (cur_block) {
    // Check for variables in this block.
    if (auto* var = SearchVariableVector(cur_block->variables(), *name))
      return FoundVariable(var);

    if (const Function* function = cur_block->AsFunction()) {
      // Found a function, check for a match in its parameters.
      if (auto* var = SearchVariableVector(function->parameters(), *name))
        return FoundVariable(var);
      break;  // Don't recurse into higher levels of nesting than a function.
    }
    if (!cur_block->parent())
      break;
    cur_block = cur_block->parent().Get()->AsCodeBlock();
  }
  return std::nullopt;
}

std::optional<FoundMember> FindMember(const Collection* object,
                                      const Identifier& identifier) {
  // TODO(brettw) allow "BaseClass::foo" syntax for specifically naming a
  // member of a base class. Watch out: the base class could be qualified (or
  // not) in various ways: ns::BaseClass::foo, BaseClass::foo, etc.
  const std::string* ident_name = identifier.GetSingleComponentName();
  if (!ident_name)
    return std::nullopt;

  // This code will check the object and all base classes.
  std::optional<FoundMember> result;
  VisitClassHierarchy(
      object, [ident_name, &result](const Collection* cur_collection,
                                    uint32_t cur_offset) -> bool {
        // Called for each collection in the hierarchy.
        for (const auto& lazy : cur_collection->data_members()) {
          const DataMember* data = lazy.Get()->AsDataMember();
          if (data && data->GetAssignedName() == *ident_name) {
            result.emplace(data, cur_offset + data->member_location());
            return true;
          }
        }
        return false;  // Not found in this scope, continue search.
      });
  return result;
}

std::optional<FoundVariable> FindMemberOnThis(const CodeBlock* block,
                                              const Identifier& identifier) {
  // Find the function to see if it has a |this| pointer.
  const Function* function = block->GetContainingFunction();
  if (!function || !function->object_pointer())
    return std::nullopt;  // No "this" pointer.

  // The "this" variable.
  const Variable* this_var = function->object_pointer().Get()->AsVariable();
  if (!this_var)
    return std::nullopt;  // Symbols likely corrupt.

  // Pointed-to type for "this".
  const Collection* collection = nullptr;
  if (GetPointedToCollection(this_var->type().Get()->AsType(), &collection)
          .has_error())
    return std::nullopt;  // Symbols likely corrupt.

  if (auto member = FindMember(collection, identifier))
    return FoundVariable(this_var, std::move(*member));
  return std::nullopt;
}

std::optional<FoundVariable> FindGlobalVariable(
    const ProcessSymbols* process_symbols, const CodeBlock* block,
    const SymbolContext* block_symbol_context, const Identifier& identifier) {
  std::vector<const LoadedModuleSymbols*> modules =
      process_symbols->GetLoadedModuleSymbols();
  if (modules.empty())
    return std::nullopt;

  Identifier current_scope;

  // When we're given a block to start searching from, always search
  // that module for symbol matches first. If there are duplicates in other
  // modules, one normally wants the current one.
  const LoadedModuleSymbols* current_module = nullptr;
  if (block) {
    // If block is non-null, so must be the symbol context.
    FXL_DCHECK(block_symbol_context);

    // Get the scope for the current block. This may fail in which case we'll
    // be left with an empty current scope. This is non-fatal so we can ignore
    // the err: it must means we won't implicitly search the current namespace.
    Err err;
    std::tie(err, current_scope) = Identifier::FromString(block->GetFullName());
    current_scope = current_scope.GetScope();

    // There's not currently a great way to map a symbol (the code block) back
    // to the module it came from. So use the symbol_context to find the module
    // that corresponds to its base address.
    uint64_t block_module_load_address =
        block_symbol_context->RelativeToAbsolute(0);
    for (const LoadedModuleSymbols* mod : modules) {
      if (mod->load_address() == block_module_load_address) {
        current_module = mod;
        break;
      }
    }

    if (current_module) {
      // Search the current module.
      if (auto found = FindGlobalVariableInModule(
              current_module->module_symbols(), current_scope, identifier))
        return found;
    }
  }

  // Search all non-current modules.
  for (const LoadedModuleSymbols* loaded_mod : modules) {
    if (loaded_mod != current_module) {
      if (auto found = FindGlobalVariableInModule(
              current_module->module_symbols(), current_scope, identifier))
        return found;
    }
  }
  return std::nullopt;
}

std::optional<FoundVariable> FindGlobalVariableInModule(
    const ModuleSymbols* module_symbols, const Identifier& current_scope,
    const Identifier& identifier) {
  IndexWalker walker(&module_symbols->GetIndex());
  if (!identifier.InGlobalNamespace()) {
    // Unless the input identifier is fully qualifed, start the search in the
    // current context.
    walker.WalkIntoClosest(current_scope);
  }

  // Search from the current namespace going up.
  do {
    IndexWalker query_walker(walker);
    if (query_walker.WalkInto(identifier)) {
      // Found a match, see if it's actually a variable we can return.
      const ModuleSymbolIndexNode* match = query_walker.current();
      if (auto found = GetVariableFromDieList(module_symbols, match->dies()))
        return found;
    }
    // No variable match, move up one level of scope and try again.
  } while (walker.WalkUp());

  return std::nullopt;
}

}  // namespace zxdb
