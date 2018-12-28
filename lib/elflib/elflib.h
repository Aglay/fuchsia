// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_ELFLIB_ELFLIB_H_
#define GARNET_LIB_ELFLIB_ELFLIB_H_

#include <map>
#include <memory>
#include <vector>

#include "garnet/third_party/llvm/include/llvm/BinaryFormat/ELF.h"
#include "garnet/public/lib/fxl/macros.h"

namespace elflib {

using namespace llvm::ELF;

class ElfLib {
 public:
  // Proxy object for whatever address space we're exploring.
  class MemoryAccessor {
   public:
    virtual ~MemoryAccessor() = default;
    // Get memory from the process relative to the base of this lib. That means
    // offset 0 should point to the Elf64_Ehdr. The vector should be sized to
    // the amount of data you want to read.
    virtual bool GetMemory(uint64_t offset, std::vector<uint8_t>* out) = 0;

    // Get memory for a mapped area. This is the same as GetMemory except we
    // are also given the target address of the memory we want according to the
    // ELF file. If we're reading ELF structures that have been mapped into a
    // running process already we may want to check the mapped address instead.
    virtual bool GetMappedMemory(uint64_t offset, uint64_t mapped_address,
                                 std::vector<uint8_t>* out) {
      return GetMemory(offset, out);
    }
  };

  // Do not use. See Create.
  explicit ElfLib(std::unique_ptr<MemoryAccessor>&& memory);

  virtual ~ElfLib();

  // Get the contents of a section by its name. Return nullptr if there is no
  // section by that name.
  const std::vector<uint8_t>* GetSectionData(const std::string& name);

  // Get the stored value of a given symbol. Returns true unless the lookup
  // failed.
  bool GetSymbolValue(const std::string& name, uint64_t* out);

  // Get a map of all symbols and their string names.
  bool GetAllSymbols(std::map<std::string,Elf64_Sym>* out);

  // Create a new ElfLib object.
  static std::unique_ptr<ElfLib> Create(
    std::unique_ptr<MemoryAccessor>&& memory);

 private:
  // Get the header for a section by its index. Return nullptr if the index is
  // invalid.
  const Elf64_Shdr* GetSectionHeader(size_t section);

  // Get the contents of a section by its index. Return nullptr if the index is
  // invalid.
  const std::vector<uint8_t>* GetSectionData(size_t section);

  // Get a string from the .strtab section. Return nullptr if the index is
  // invalid.
  const std::string* GetString(size_t index);

  // Get a symbol from the symbol table. Return nullptr if there is no such
  // symbol.
  const Elf64_Sym* GetSymbol(const std::string& name);

  // Load all symbols from the target. Returns true unless an error occurred.
  bool LoadSymbols();

  std::unique_ptr<MemoryAccessor> memory_;
  Elf64_Ehdr header_;
  std::vector<Elf64_Shdr> sections_;
  std::vector<Elf64_Sym> symbols_;
  std::vector<std::string> strings_;
  std::map<size_t, std::vector<uint8_t>> section_data_;
  std::map<std::string, size_t> section_names_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ElfLib);
};

}  // namespace elflib

#endif  // GARNET_LIB_ELFLIB_ELFLIB_H_
