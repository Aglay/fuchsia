// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/symbols/build_id_index.h"

#include <algorithm>

#include "garnet/lib/debug_ipc/helper/elf.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/fxl/strings/string_view.h"
#include "lib/fxl/strings/trim.h"

namespace zxdb {

namespace {

std::filesystem::path BuildIdToDebugPath(const std::string& build_id) {
  std::string build_id_clean = build_id;

  build_id_clean.erase(
      std::remove(build_id_clean.begin(), build_id_clean.end(), '-'),
      build_id_clean.end());
  std::transform(build_id_clean.begin(), build_id_clean.end(),
                 build_id_clean.begin(), ::tolower);

  if (build_id_clean.length() < 3) {
    return std::filesystem::path();
  }

  const std::string& folder = build_id_clean.substr(0, 2);
  const std::string& file =
      build_id_clean.substr(2, build_id_clean.size() - 2) + ".debug";

  return std::filesystem::path(folder) / file;
}

}  // namespace

BuildIDIndex::BuildIDIndex() = default;
BuildIDIndex::~BuildIDIndex() = default;

std::string BuildIDIndex::FileForBuildID(const std::string& build_id) {
  EnsureCacheClean();

  auto found = build_id_to_file_.find(build_id);
  if (found == build_id_to_file_.end())
    return SearchRepoSources(build_id);
  return found->second;
}

std::string BuildIDIndex::SearchRepoSources(const std::string& build_id) {
  auto local_path = BuildIdToDebugPath(build_id);

  if (local_path.empty()) {
    return std::string();
  }

  for (const auto& source : repo_sources_) {
    const auto& path = std::filesystem::path(source) / ".build-id" / local_path;

    if (std::filesystem::exists(path)) {
      return path;
    }
  }

  return std::string();
}

void BuildIDIndex::AddBuildIDMapping(const std::string& build_id,
                                     const std::string& file_name) {
  // This map saves the manual mapping across cache updates.
  manual_mappings_[build_id] = file_name;

  // Don't bother marking the cache dirty since we can just add it.
  build_id_to_file_[build_id] = file_name;
}

void BuildIDIndex::AddBuildIDMappingFile(const std::string& id_file_name) {
  // If the file is already loaded, ignore it.
  if (std::find(build_id_files_.begin(), build_id_files_.end(), id_file_name) !=
      build_id_files_.end())
    return;

  build_id_files_.emplace_back(id_file_name);
  ClearCache();
}

void BuildIDIndex::AddSymbolSource(const std::string& path) {
  // If the file is already loaded, ignore it.
  if (std::find(sources_.begin(), sources_.end(), path) != sources_.end())
    return;

  sources_.emplace_back(path);
  ClearCache();
}

void BuildIDIndex::AddRepoSymbolSource(const std::string& path) {
  repo_sources_.emplace_back(path);
  EnsureCacheClean();
}

BuildIDIndex::StatusList BuildIDIndex::GetStatus() {
  EnsureCacheClean();
  return status_;
}

void BuildIDIndex::ClearCache() {
  build_id_to_file_.clear();
  status_.clear();
  cache_dirty_ = true;
}

// static
int BuildIDIndex::ParseIDs(const std::string& input,
                           const std::filesystem::path& containing_dir,
                           IDMap* output) {
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
        fxl::StringView path_data = fxl::TrimString(
            line.substr(first_space + 1, line.size() - first_space - 1),
            to_trim);

        std::filesystem::path path(path_data.ToString());

        if (path.is_relative()) {
          path = containing_dir / path;
        }

        added++;
        output->emplace(std::piecewise_construct,
                        std::forward_as_tuple(build_id.data(), build_id.size()),
                        std::forward_as_tuple(path));
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
  std::error_code err;

  auto path = std::filesystem::canonical(file_name, err);

  if (err) {
    status_.emplace_back(file_name, 0);
    LogMessage("Can't open build ID file: " + file_name);
    return;
  }

  auto containing_dir = path.parent_path();

  FILE* id_file = fopen(file_name.c_str(), "r");
  if (!id_file) {
    status_.emplace_back(file_name, 0);
    LogMessage("Can't open build ID file: " + file_name);
    return;
  }

  fseek(id_file, 0, SEEK_END);
  long length = ftell(id_file);
  if (length <= 0) {
    status_.emplace_back(file_name, 0);
    LogMessage("Can't load build ID file: " + file_name);
    return;
  }

  std::string contents;
  contents.resize(length);

  fseek(id_file, 0, SEEK_SET);
  if (fread(&contents[0], 1, contents.size(), id_file) !=
      static_cast<size_t>(length)) {
    status_.emplace_back(file_name, 0);
    LogMessage("Can't read build ID file: " + file_name);
    return;
  }

  fclose(id_file);

  int added = ParseIDs(contents, containing_dir, &build_id_to_file_);
  status_.emplace_back(file_name, added);
  if (!added)
    LogMessage("No mappings found in build ID file: " + file_name);
}

void BuildIDIndex::IndexOneSourcePath(const std::string& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    // Iterate through all files in this directory, but don't recurse.
    int indexed = 0;
    for (const auto& child : std::filesystem::directory_iterator(path)) {
      if (IndexOneSourceFile(child.path()))
        indexed++;
    }
    status_.emplace_back(path, indexed);
  } else if (!ec) {
    if (IndexOneSourceFile(path)) {
      status_.emplace_back(path, 1);
    } else {
      status_.emplace_back(path, 0);
      LogMessage(fxl::StringPrintf("Symbol file could not be loaded: %s",
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

  for (const auto& path : repo_sources_)
    status_.emplace_back(path, BuildIDIndex::kStatusIsFolder);

  cache_dirty_ = false;
}

}  // namespace zxdb
