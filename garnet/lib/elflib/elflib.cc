// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <string>

#include <lib/fxl/logging.h>

#include "garnet/lib/elflib/elflib.h"

namespace elflib {
namespace {

// Pull a null-terminated string out of an array of bytes at an offset. Returns
// empty string if there is no null terminator.
std::string GetNullTerminatedStringAt(const std::vector<uint8_t>& data,
                                      size_t offset) {
  size_t check = offset;

  while (check < data.size() && data[check]) {
    check++;
  }

  if (check >= data.size()) {
    return std::string();
  }

  const char* start = reinterpret_cast<const char*>(data.data()) + offset;

  return std::string(start);
}

}  // namespace

ElfLib::ElfLib(std::unique_ptr<MemoryAccessor>&& memory)
    : memory_(std::move(memory)) {}

ElfLib::~ElfLib() = default;

std::unique_ptr<ElfLib> ElfLib::Create(
    std::unique_ptr<MemoryAccessor>&& memory) {
  std::unique_ptr<ElfLib> out = std::make_unique<ElfLib>(std::move(memory));
  auto header_data = out->memory_->GetMemory(0, sizeof(Elf64_Ehdr));

  if (!header_data) {
    return std::unique_ptr<ElfLib>();
  }

  out->header_ = *reinterpret_cast<Elf64_Ehdr*>(header_data->data());

  // We don't support non-standard section header sizes. Stripped binaries that
  // don't have sections sometimes zero out the shentsize, so we can ignore it
  // if we have no sections.
  if (out->header_.e_shnum > 0 &&
      out->header_.e_shentsize != sizeof(Elf64_Shdr)) {
    return std::unique_ptr<ElfLib>();
  }

  // We don't support non-standard program header sizes.
  if (out->header_.e_phentsize != sizeof(Elf64_Phdr)) {
    return std::unique_ptr<ElfLib>();
  }

  return out;
}

const Elf64_Shdr* ElfLib::GetSectionHeader(size_t section) {
  if (sections_.empty()) {
    auto data = memory_->GetMemory(header_.e_shoff,
                                   sizeof(Elf64_Shdr) * header_.e_shnum);

    if (!data) {
      return nullptr;
    }

    Elf64_Shdr* header_array = reinterpret_cast<Elf64_Shdr*>(data->data());

    std::copy(header_array, header_array + header_.e_shnum,
              std::back_inserter(sections_));
  }

  if (section >= sections_.size()) {
    return nullptr;
  }

  return &sections_[section];
}

bool ElfLib::LoadProgramHeaders() {
  if (!segments_.empty()) {
    return true;
  }

  auto data =
      memory_->GetMemory(header_.e_phoff, sizeof(Elf64_Phdr) * header_.e_phnum);

  if (!data) {
    return false;
  }

  Elf64_Phdr* header_array = reinterpret_cast<Elf64_Phdr*>(data->data());

  std::copy(header_array, header_array + header_.e_phnum,
            std::back_inserter(segments_));

  return true;
}

const std::vector<uint8_t>* ElfLib::GetSegmentData(size_t segment) {
  const auto& iter = segment_data_.find(segment);
  if (iter != segment_data_.end()) {
    return &iter->second;
  }

  LoadProgramHeaders();

  if (segment > segments_.size()) {
    return nullptr;
  }

  const Elf64_Phdr* header = &segments_[segment];

  auto data = memory_->GetMappedMemory(header->p_offset, header->p_vaddr,
                                       header->p_filesz, header->p_memsz);

  if (!data) {
    return nullptr;
  }

  segment_data_[segment] = *data;

  return &segment_data_[segment];
}

const std::optional<std::vector<uint8_t>> ElfLib::GetNote(
    const std::string& name, uint64_t type) {
  LoadProgramHeaders();

  for (size_t idx = 0; idx < segments_.size(); idx++) {
    if (segments_[idx].p_type != PT_NOTE) {
      continue;
    }

    auto data = GetSegmentData(idx);

    const Elf64_Nhdr* header;
    size_t namesz_padded;
    size_t descsz_padded;

    for (const uint8_t* pos = data->data(); pos < data->data() + data->size();
         pos += sizeof(Elf64_Nhdr) + namesz_padded + descsz_padded) {
      header = reinterpret_cast<const Elf64_Nhdr*>(pos);
      namesz_padded = (header->n_namesz + 3) & ~3UL;
      descsz_padded = (header->n_descsz + 3) & ~3UL;

      if (header->n_type != type) {
        continue;
      }

      auto name_data = pos + sizeof(Elf64_Nhdr);
      std::string entry_name(reinterpret_cast<const char*>(name_data),
                             header->n_namesz - 1);

      if (entry_name == name) {
        auto desc_data = name_data + namesz_padded;

        return std::vector(desc_data, desc_data + header->n_descsz);
      }
    }
  }

  return std::nullopt;
}

const std::vector<uint8_t>* ElfLib::GetSectionData(size_t section) {
  const auto& iter = section_data_.find(section);
  if (iter != section_data_.end()) {
    return &iter->second;
  }

  const Elf64_Shdr* header = GetSectionHeader(section);

  if (!header) {
    return nullptr;
  }

  auto data = memory_->GetMappedMemory(header->sh_offset, header->sh_addr,
                                       header->sh_size, header->sh_size);

  if (!data) {
    return nullptr;
  }

  section_data_[section] = *data;

  return &section_data_[section];
}

const std::vector<uint8_t>* ElfLib::GetSectionData(const std::string& name) {
  if (section_names_.size() == 0) {
    const std::vector<uint8_t>* section_name_data =
        GetSectionData(header_.e_shstrndx);

    if (!section_name_data) {
      return nullptr;
    }

    size_t idx = 0;
    // We know sections_ is populated from the GetSectionData above
    for (const auto& section : sections_) {
      auto name =
          GetNullTerminatedStringAt(*section_name_data, section.sh_name);
      section_names_[name] = idx;

      idx++;
    }
  }

  const auto& iter = section_names_.find(name);

  if (iter == section_names_.end()) {
    return nullptr;
  }

  return GetSectionData(iter->second);
}

bool ElfLib::LoadDynamicSymbols() {
  if (dynamic_symtab_offset_ || dynamic_strtab_offset_) {
    return true;
  }

  LoadProgramHeaders();

  for (size_t idx = 0; idx < segments_.size(); idx++) {
    if (segments_[idx].p_type != PT_DYNAMIC) {
      continue;
    }

    auto data = GetSegmentData(idx);

    if (!data) {
      return false;
    }

    const Elf64_Dyn* start = reinterpret_cast<const Elf64_Dyn*>(data->data());
    const Elf64_Dyn* end = start + (data->size() / sizeof(Elf64_Dyn));

    dynamic_strtab_size_ = 0;
    dynamic_symtab_size_ = 0;

    for (auto dyn = start; dyn != end; dyn++) {
      if (dyn->d_tag == DT_STRTAB) {
        if (dynamic_strtab_offset_) {
          // We have more than one entry specifying the strtab location. Not
          // clear what to do there so just ignore all but the first.
          continue;
        }

        dynamic_strtab_offset_ = dyn->d_un.d_ptr;
      } else if (dyn->d_tag == DT_SYMTAB) {
        if (dynamic_symtab_offset_) {
          continue;
        }

        dynamic_symtab_offset_ = dyn->d_un.d_ptr;
      } else if (dyn->d_tag == DT_STRSZ) {
        if (dynamic_strtab_size_) {
          continue;
        }

        dynamic_strtab_size_ = dyn->d_un.d_val;
      } else if (dyn->d_tag == DT_HASH) {
        // A note: The old DT_HASH style of hash table is considered legacy on
        // Fuchsia. Technically a binary could provide both styles of hash
        // table and we can produce a sane result in that case, so this code
        // ignores DT_HASH.
        FXL_LOG(WARNING) << "Old style DT_HASH table found.";
      } else if (dyn->d_tag == DT_GNU_HASH) {
        auto addr = dyn->d_un.d_ptr;

        // Our elf header doesn't provide the DT_GNU_HASH header structure.
        struct Header {
          uint32_t nbuckets;
          uint32_t symoffset;
          uint32_t bloom_size;
          uint32_t bloom_shift;
        } header;

        static_assert(sizeof(Header) == 16);

        auto data = memory_->GetMappedMemory(addr, addr, sizeof(header),
                                             sizeof(header));

        if (!data) {
          continue;
        }

        header = *reinterpret_cast<Header*>(data->data());

        addr += sizeof(header);
        addr += 8 * header.bloom_size;

        size_t bucket_bytes = 4 * header.nbuckets;
        auto bucket_data =
            memory_->GetMappedMemory(addr, addr, bucket_bytes, bucket_bytes);

        if (!bucket_data) {
          continue;
        }

        uint32_t* buckets = reinterpret_cast<uint32_t*>(bucket_data->data());
        uint32_t max_bucket =
            *std::max_element(buckets, buckets + header.nbuckets);

        if (max_bucket < header.symoffset) {
          dynamic_symtab_size_ = max_bucket;
          continue;
        }

        addr += bucket_bytes;
        addr += (max_bucket - header.symoffset) * 4;

        for (uint32_t nsyms = max_bucket + 1;; nsyms++, addr += 4) {
          auto chain_entry_data = memory_->GetMappedMemory(addr, addr, 4, 4);

          if (!chain_entry_data) {
            break;
          }

          uint32_t chain_entry =
              *reinterpret_cast<uint32_t*>(chain_entry_data->data());

          if (chain_entry & 1) {
            dynamic_symtab_size_ = nsyms;
            break;
          }
        }
      }
    }

    return true;
  }

  return false;
}

std::optional<std::string> ElfLib::GetString(size_t index) {
  std::vector<uint8_t> tmp;
  const std::vector<uint8_t>* string_data = GetSectionData(".strtab");

  if (!string_data) {
    if (!LoadDynamicSymbols()) {
      return std::nullopt;
    }

    auto data = memory_->GetMappedMemory(
        *dynamic_strtab_offset_, *dynamic_strtab_offset_, dynamic_strtab_size_,
        dynamic_strtab_size_);

    if (!data) {
      return std::nullopt;
    }

    tmp = *data;
    string_data = &tmp;
  }

  return GetNullTerminatedStringAt(*string_data, index);
}

bool ElfLib::LoadSymbols() {
  if (symbols_.empty()) {
    std::vector<uint8_t> tmp;
    const std::vector<uint8_t>* symbol_data = GetSectionData(".symtab");

    if (!symbol_data) {
      if (!LoadDynamicSymbols()) {
        return false;
      }

      if (!dynamic_symtab_offset_) {
        return false;
      }

      size_t size = dynamic_symtab_size_ * sizeof(Elf64_Sym);
      auto data = memory_->GetMappedMemory(*dynamic_symtab_offset_,
                                           *dynamic_symtab_offset_, size, size);

      if (!data) {
        return false;
      }

      tmp = *data;
      symbol_data = &tmp;
    }

    const Elf64_Sym* start =
        reinterpret_cast<const Elf64_Sym*>(symbol_data->data());
    const Elf64_Sym* end = start + (symbol_data->size() / sizeof(Elf64_Sym));
    std::copy(start, end, std::back_inserter(symbols_));
  }

  return true;
}

const Elf64_Sym* ElfLib::GetSymbol(const std::string& name) {
  if (!LoadSymbols()) {
    return nullptr;
  }

  for (const auto& symbol : symbols_) {
    auto got_name = GetString(symbol.st_name);

    if (got_name && *got_name == name) {
      return &symbol;
    }
  }

  return nullptr;
}

std::optional<std::map<std::string, Elf64_Sym>> ElfLib::GetAllSymbols() {
  if (!LoadSymbols()) {
    return std::nullopt;
  }

  std::map<std::string, Elf64_Sym> out;

  for (const auto& symbol : symbols_) {
    auto got_name = GetString(symbol.st_name);

    if (got_name) {
      out[*got_name] = symbol;
    }
  }

  return out;
}

std::optional<uint64_t> ElfLib::GetSymbolValue(const std::string& name) {
  const Elf64_Sym* sym = GetSymbol(name);

  if (sym) {
    return sym->st_value;
  }

  return std::nullopt;
}

}  // namespace elflib
