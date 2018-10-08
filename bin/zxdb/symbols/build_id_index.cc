// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/symbols/build_id_index.h"

#include <filesystem>

#include "garnet/lib/debug_ipc/helper/elf.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/fxl/strings/string_view.h"
#include "lib/fxl/strings/trim.h"

namespace zxdb {

BuildIDIndex::BuildIDIndex() = default;
BuildIDIndex::~BuildIDIndex() = default;

std::string BuildIDIndex::FileForBuildID(const std::string& build_id) {
  EnsureCacheClean();

  auto found = build_id_to_file_.find(build_id);
  if (found == build_id_to_file_.end())
    return std::string();
  return found->second;
}

void BuildIDIndex::AddBuildIDMapping(const std::string& build_id,
                                     const std::string& file_name) {
  // This map saves the manual mapping across cache updates.
  manual_mappings_[build_id] = file_name;

  // Don't bother marking the cache dirty since we can just add it.
  build_id_to_file_[build_id] = file_name;
}

void BuildIDIndex::AddBuildIDMappingFile(const std::string& id_file_name) {
  build_id_files_.emplace_back(id_file_name);
  ClearCache();
}

void BuildIDIndex::AddSymbolSource(const std::string& path) {
  sources_.emplace_back(path);
  ClearCache();
}

void BuildIDIndex::ClearCache() {
  build_id_to_file_.clear();
  cache_dirty_ = true;
}

// static
int BuildIDIndex::ParseIDs(const std::string& input, IDMap* output) {
  int added = 0;
  for (size_t line_begin = 0; line_begin < input.size(); line_begin++) {
    size_t newline = input.find('\n', line_begin);
    if (newline == std::string::npos)
      newline = input.size();

    fxl::StringView line(&input[line_begin], newline - line_begin);
    if (!line.empty()) {
      // Format is <buildid> <space> <filename>
      size_t first_space = line.find(' ');
      if (first_space != std::string::npos && first_space > 0 &&
          first_space + 1 < line.size()) {
        // There is a space and it separates two nonempty things.
        fxl::StringView to_trim(" \t\r\n");
        fxl::StringView build_id =
            fxl::TrimString(line.substr(0, first_space), to_trim);
        fxl::StringView path = fxl::TrimString(
            line.substr(first_space + 1, line.size() - first_space - 1),
            to_trim);

        added++;
        output->emplace(std::piecewise_construct,
                        std::forward_as_tuple(build_id.data(), build_id.size()),
                        std::forward_as_tuple(path.data(), path.size()));
      }
    }

    line_begin = newline;  // The for loop will advance past this.
  }
  return added;
}

void BuildIDIndex::LogMessage(const std::string& msg) const {
  if (information_callback_)
    information_callback_(msg);
}

void BuildIDIndex::LoadOneBuildIDFile(const std::string& file_name) {
  FILE* id_file = fopen(file_name.c_str(), "r");
  if (!id_file) {
    LogMessage("Can't open build ID file: " + file_name);
    return;
  }

  fseek(id_file, 0, SEEK_END);
  long length = ftell(id_file);
  if (length <= 0) {
    LogMessage("Can't load build ID file: " + file_name);
    return;
  }

  std::string contents;
  contents.resize(length);

  fseek(id_file, 0, SEEK_SET);
  if (fread(&contents[0], 1, contents.size(), id_file) !=
      static_cast<size_t>(length)) {
    LogMessage("Can't read build ID file: " + file_name);
    return;
  }

  fclose(id_file);

  int added = ParseIDs(contents, &build_id_to_file_);
  if (!added) {
    LogMessage("No mappings found in build ID file: " + file_name);
  } else {
    LogMessage(fxl::StringPrintf("Loaded %d system symbol mappings from:\n  %s",
                                 added, file_name.c_str()));
  }
}

void BuildIDIndex::IndexOneSourcePath(const std::string& path) {
  if (std::filesystem::is_directory(path)) {
    // Iterate through all files in this directory, but don't recurse.
    int indexed = 0;
    for (const auto& child : std::filesystem::directory_iterator(path)) {
      if (IndexOneSourceFile(child.path()))
        indexed++;
    }
    LogMessage(fxl::StringPrintf("Loaded %d symbol paddings from:\n  %s",
                                 indexed, path.c_str()));
  } else {
    if (!IndexOneSourceFile(path)) {
      LogMessage(fxl::StringPrintf("Symbol file could not be loaded:\n  %s",
                                   path.c_str()));
    }
  }
}

bool BuildIDIndex::IndexOneSourceFile(const std::string& file_path) {
  FILE* file = fopen(file_path.c_str(), "rb");
  if (!file)
    return false;
  std::string build_id = debug_ipc::ExtractBuildID(file);
  fclose(file);

  if (!build_id.empty()) {
    build_id_to_file_[build_id] = file_path;
    return true;
  }
  return false;
}

void BuildIDIndex::EnsureCacheClean() {
  if (!cache_dirty_)
    return;

  for (const auto& build_id_file : build_id_files_)
    LoadOneBuildIDFile(build_id_file);

  for (const auto& source : sources_)
    IndexOneSourcePath(source);

  for (const auto& mapping : manual_mappings_)
    build_id_to_file_.insert(mapping);

  cache_dirty_ = false;
}

}  // namespace zxdb
