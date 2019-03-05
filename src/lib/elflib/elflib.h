// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_ELFLIB_ELFLIB_H_
#define GARNET_LIB_ELFLIB_ELFLIB_H_

#include <stdio.h>

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "garnet/public/lib/fxl/macros.h"
#include "garnet/third_party/llvm/include/llvm/BinaryFormat/ELF.h"

namespace elflib {

using namespace llvm::ELF;

class ElfLib {
 public:
  class MemoryAccessor;

  // Essentially just a pointer with a bound.
  struct MemoryRegion {
    const uint8_t* ptr;
    size_t size;
  };

  // How do we expect the ELF structures to be mapped? Are they packed in a
  // file or mapped as they would be in a running process?
  enum class AddressMode { kFile, kProcess };

  // Whether we should take ownership of the FILE handle given to our Create
  // method.
  enum class Ownership { kTakeOwnership, kDontTakeOwnership };

  // Do not use. See Create.
  explicit ElfLib(std::unique_ptr<MemoryAccessor>&& memory,
                  AddressMode address_mode);

  virtual ~ElfLib();

  // Get the contents of a section by its name. Return nullptr if there is no
  // section by that name.
  MemoryRegion GetSectionData(const std::string& name);

  // Get a note from the notes section.
  std::optional<std::vector<uint8_t>> GetNote(const std::string& name,
                                              uint64_t type);

  // Get a symbol from the symbol table. Return nullptr if there is no such
  // symbol. Pointer should live as long as the memory accessor.
  const Elf64_Sym* GetSymbol(const std::string& name);

  // Get a map of all symbols and their string names. Returns nullopt if the
  // symbols could not be loaded.
  std::optional<std::map<std::string, Elf64_Sym>> GetAllSymbols();

  // Get a symbol from the symbol table. Return nullptr if there is no such
  // symbol. Pointer should live as long as the memory accessor.
  const Elf64_Sym* GetDynamicSymbol(const std::string& name);

  // Get a map of all symbols and their string names. Returns nullopt if the
  // symbols could not be loaded.
  std::optional<std::map<std::string, Elf64_Sym>> GetAllDynamicSymbols();

  // Create a new ElfLib object for reading a file. If take_ownership is set to
  // true, the given handle will be closed when the ElfLib object is destroyed.
  static std::unique_ptr<ElfLib> Create(FILE* fp, Ownership owned);

  // Create a new ElfLib object for reading a file. ElfLib will attempt to open
  // the file and retain a handle to it until the object is destroyed.
  static std::unique_ptr<ElfLib> Create(const std::string& path);

  // Create a new ElfLib object for accessing an ELF file mapped into memory.
  // This is expected to be a file, not an address space, and will be addressed
  // accordingly.
  static std::unique_ptr<ElfLib> Create(const uint8_t* mem, size_t size);

  // Create an ElfLib object for reading ELF structures via a read callback.
  // The offsets will assume either an ELF file or an ELF mapped address space
  // depending on the value of the address_mode argument.
  static std::unique_ptr<ElfLib> Create(
      std::function<bool(uint64_t, std::vector<uint8_t>*)> fetch,
      AddressMode address_mode = AddressMode::kProcess);

  // Returns a map from symbol names to the locations of their PLT entries.
  // Returns an empty map if the data is inaccessible.
  //
  // Getting this information is architecture-specific and involves reading and
  // decoding the actual jump table instructions in the .plt section. Once
  // we've done that decoding we can quickly get relocation indices and then
  // symbol table mappings.
  std::map<std::string, uint64_t> GetPLTOffsets();

 private:
  // Location of a section specified by data gleaned from the dynamic segment.
  struct DynamicSection {
    std::optional<uint64_t> offset;
    std::optional<size_t> size;

    bool IsValid() { return offset && size; }
  };

  // Create a new ElfLib object.
  static std::unique_ptr<ElfLib> Create(
      std::unique_ptr<MemoryAccessor>&& memory, AddressMode address_mode);

  // x64-specific implementation of GetPLTOffsets
  std::map<std::string, uint64_t> GetPLTOffsetsX64();

  // Get the header for a section by its index. Return nullptr if the index is
  // invalid.
  const Elf64_Shdr* GetSectionHeader(size_t section);

  // Load the program header table into the cache in segments_. Return true
  // unless a read error occurred.
  bool LoadProgramHeaders();

  // Load the section name-to-index mappings and cache them in section_names_.
  bool LoadSectionNames();

  // Get the contents of a section by its index. Return nullptr if the index is
  // invalid.
  MemoryRegion GetSectionData(size_t section);

  // Get the contents of a segment by its index. Return nullptr if the index is
  // invalid.
  MemoryRegion GetSegmentData(size_t segment);

  const AddressMode address_mode_;

  // Get the contents of the symbol table. Return nullptr if it is not present
  // or we do not have the means to locate it. Size is number of structs, not
  // number of bytes.
  std::pair<const Elf64_Sym*, size_t> GetSymtab();

  // Get the contents of the dynamic symbol table. Return nullptr if it is not
  // present or we do not have the means to locate it. Size is number of
  // structs, not number of bytes.
  std::pair<const Elf64_Sym*, size_t> GetDynamicSymtab();

  // Get a string from the .strtab section. Return nullptr if the offset is
  // invalid.
  std::optional<std::string> GetString(size_t offset);

  // Get a string from the .dynstr section. Return nullptr if the offset is
  // invalid.
  std::optional<std::string> GetDynamicString(size_t offset);

  // Load symbols from the dynamic segment of the target. We only do this when
  // the section data isn't available and we can't use the regular .symtab
  // information. Returns true unless an error occurred.
  bool LoadDynamicSymbols();

  bool did_load_dynamic_symbols_ = false;

  std::unique_ptr<MemoryAccessor> memory_;
  Elf64_Ehdr header_;
  std::optional<bool> dynamic_plt_use_rela_;

  DynamicSection dynsym_;
  DynamicSection dynstr_;
  std::vector<Elf64_Shdr> sections_;
  std::vector<Elf64_Phdr> segments_;
  std::map<size_t, std::vector<uint8_t>> section_data_;
  std::map<std::string, size_t> section_names_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ElfLib);
};

}  // namespace elflib

#endif  // GARNET_LIB_ELFLIB_ELFLIB_H_
