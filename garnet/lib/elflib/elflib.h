// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_ELFLIB_ELFLIB_H_
#define GARNET_LIB_ELFLIB_ELFLIB_H_

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "garnet/public/lib/fxl/macros.h"
#include "garnet/third_party/llvm/include/llvm/BinaryFormat/ELF.h"

namespace elflib {

using namespace llvm::ELF;

class ElfLib {
 public:
  // Proxy object for whatever address space we're exploring.
  class MemoryAccessor {
   public:
    virtual ~MemoryAccessor() = default;

    // Retrieve the header for this ELF object. Create() will fail if this
    // returns an empty optional.
    virtual std::optional<Elf64_Ehdr> GetHeader() = 0;

    // Retrieve the section header table. The recorded offset and size are
    // passed. The callee just has to derference.
    virtual std::optional<std::vector<Elf64_Shdr>> GetSectionHeaders(
        uint64_t offset, size_t count) = 0;

    // Retrieve the program header table. The recorded offset and size are
    // passed. The callee just has to dereference.
    virtual std::optional<std::vector<Elf64_Phdr>> GetProgramHeaders(
        uint64_t offset, size_t count) = 0;

    // Get memory for a mapped area as specified by a section or segment. We're
    // given the dimensions both as we'd find them in the file and as we'd find
    // them in address space.
    virtual std::optional<std::vector<uint8_t>> GetLoadableMemory(
        uint64_t offset, uint64_t mapped_address, size_t file_size,
        size_t mapped_size) = 0;

    // Get memory for a mapped area specified by a segment. The memory is
    // assumed to only be accessible at the desired address after loading and
    // you should return std::nullopt if this isn't a loaded ELF object.
    virtual std::optional<std::vector<uint8_t>> GetLoadedMemory(
        uint64_t mapped_address, size_t mapped_size) = 0;
  };

  class MemoryAccessorForFile : public MemoryAccessor {
   public:
    // Get memory from the file based on its offset.
    virtual std::optional<std::vector<uint8_t>> GetMemory(uint64_t offset,
                                                          size_t size) = 0;

    std::optional<std::vector<uint8_t>> GetLoadedMemory(
        uint64_t mapped_address, size_t mapped_size) override {
      return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> GetLoadableMemory(
        uint64_t offset, uint64_t mapped_address, size_t file_size,
        size_t mapped_size) override {
      return GetMemory(offset, file_size);
    }

    std::optional<Elf64_Ehdr> GetHeader() override;
    std::optional<std::vector<Elf64_Shdr>> GetSectionHeaders(
        uint64_t offset, size_t count) override;
    std::optional<std::vector<Elf64_Phdr>> GetProgramHeaders(
        uint64_t offset, size_t count) override;
  };

  class MemoryAccessorForAddressSpace : public MemoryAccessor {
   public:
    std::optional<std::vector<uint8_t>> GetLoadableMemory(
        uint64_t offset, uint64_t mapped_address, size_t file_size,
        size_t mapped_size) override {
      return GetLoadedMemory(mapped_address, mapped_size);
    }

    std::optional<std::vector<Elf64_Shdr>> GetSectionHeaders(
        uint64_t offset, size_t count) override {
      return std::nullopt;
    }
  };

  // Do not use. See Create.
  explicit ElfLib(std::unique_ptr<MemoryAccessor>&& memory);

  virtual ~ElfLib();

  // Get the contents of a section by its name. Return nullptr if there is no
  // section by that name.
  const std::vector<uint8_t>* GetSectionData(const std::string& name);

  // Get a note from the notes section.
  const std::optional<std::vector<uint8_t>> GetNote(const std::string& name,
                                                    uint64_t type);

  // Get the stored value of a given symbol. Returns nullopt if the lookup
  // failed.
  std::optional<uint64_t> GetSymbolValue(const std::string& name);

  // Get a map of all symbols and their string names. Returns nullopt if the
  // symbols could not be loaded.
  std::optional<std::map<std::string, Elf64_Sym>> GetAllSymbols();

  // Create a new ElfLib object.
  static std::unique_ptr<ElfLib> Create(
      std::unique_ptr<MemoryAccessor>&& memory);

 private:
  // Get the header for a section by its index. Return nullptr if the index is
  // invalid.
  const Elf64_Shdr* GetSectionHeader(size_t section);

  // Load the program header table into the cache in segments_. Return true
  // unless a read error occurred.
  bool LoadProgramHeaders();

  // Get the contents of a section by its index. Return nullptr if the index is
  // invalid.
  const std::vector<uint8_t>* GetSectionData(size_t section);

  // Get the contents of a segment by its index. Return nullptr if the index is
  // invalid.
  const std::vector<uint8_t>* GetSegmentData(size_t segment);

  // Get a string from the .strtab section. Return nullptr if the index is
  // invalid.
  std::optional<std::string> GetString(size_t index);

  // Get a symbol from the symbol table. Return nullptr if there is no such
  // symbol.
  const Elf64_Sym* GetSymbol(const std::string& name);

  // Load all symbols from the target. Returns true unless an error occurred.
  bool LoadSymbols();

  // Load symbols from the dynamic segment of the target. We only do this when
  // the section data isn't available and we can't use the regular .symtab
  // information. Returns true unless an error occurred.
  bool LoadDynamicSymbols();

  std::unique_ptr<MemoryAccessor> memory_;
  Elf64_Ehdr header_;
  size_t dynamic_strtab_size_;
  size_t dynamic_symtab_size_;
  std::optional<uint64_t> dynamic_strtab_offset_;
  std::optional<uint64_t> dynamic_symtab_offset_;
  std::vector<Elf64_Shdr> sections_;
  std::vector<Elf64_Phdr> segments_;
  std::vector<Elf64_Sym> symbols_;
  std::map<size_t, std::vector<uint8_t>> section_data_;
  std::map<size_t, std::vector<uint8_t>> segment_data_;
  std::map<std::string, size_t> section_names_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ElfLib);
};

}  // namespace elflib

#endif  // GARNET_LIB_ELFLIB_ELFLIB_H_
