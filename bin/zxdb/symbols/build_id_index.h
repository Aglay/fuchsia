// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace zxdb {

// This class maintains an index of build ID to local file path for files
// that may have symbols in them.
//
// It can get files from different sources: an explicit ID mapping file, an
// explicitly given elf file path, or a directory which it will scan for ELF
// files and index.
class BuildIDIndex {
 public:
  using IDMap = std::map<std::string, std::string>;

  // Lists symbol sources and the number of ELF files indexed at that location.
  using StatusList = std::vector<std::pair<std::string, int>>;

  BuildIDIndex();
  ~BuildIDIndex();

  // Sets the callback for informational messages. Null callbacks are legal.
  void set_information_callback(std::function<void(const std::string&)> fn) {
    information_callback_ = std::move(fn);
  }

  // Returns the local file name for the given build ID, or the empty string
  // if there is no match.
  std::string FileForBuildID(const std::string& build_id);

  // Manually inserts a mapping of
  void AddBuildIDMapping(const std::string& build_id,
                         const std::string& file_name);

  // Adds an "ids.txt" file that maps build ID to file paths.
  // Will verify that the path is already there and ignore it if so.
  void AddBuildIDMappingFile(const std::string& id_file_name);

  // Adds a file or directory to the symbol search index. If the path is a
  // file this class will try to parse it as an ELF file and add it to the
  // index if it is.
  //
  // If the path is a directory, all files in that directory will be indexed.
  //
  // Will ignore the path if it's already loaded.
  void AddSymbolSource(const std::string& path);

  // Returns the status of the symbols. This will force the cache to be fresh
  // so may cause I/O.
  StatusList GetStatus();

  // Clears all cached build IDs. They will be reloaded when required.
  void ClearCache();

  // Parses a build ID mapping file (ids.txt). This is a separate static
  // function for testing purposes. The results are added to the output.
  // Returns the number of items loaded.
  static int ParseIDs(const std::string& input, IDMap* output);

  const std::vector<std::string>& build_id_files() const {
    return build_id_files_;
  }
  const std::vector<std::string>& sources() const { return sources_; }

 private:
  // Updates the build_id_to_file_ cache if necessary.
  void EnsureCacheClean();

  // Logs an informational message.
  void LogMessage(const std::string& msg) const;

  // Adds all the mappings from the given build ID file to the index.
  void LoadOneBuildIDFile(const std::string& file_name);

  // Adds all the mappings from the given file or directory to the index.
  void IndexOneSourcePath(const std::string& path);

  // Indexes one ELF file and adds it to the index. Returns true if it was an
  // ELF file and it was added to the index.
  bool IndexOneSourceFile(const std::string& file_path);

  // Function to output informational messages. May be null. Use LogMessage().
  std::function<void(const std::string&)> information_callback_;

  std::vector<std::string> build_id_files_;

  // Either files or directories to index.
  std::vector<std::string> sources_;

  // Maintains the logs of how many symbols were indexed for each location.
  StatusList status_;

  // Indicates if build_id_to_file_ is up-to-date. This is necessary to
  // disambiguate whether an empty cache means "not scanned" or "nothing found".
  bool cache_dirty_ = true;

  // Manually-added build ID mappings. This is not cleared when the cache is
  // cleared, and these are added to the mappings when the cache is rebuilt.
  IDMap manual_mappings_;

  // Index of build IDs to local file paths.
  IDMap build_id_to_file_;
};

}  // namespace zxdb
