// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_ALLOW_LIST_H_
#define SRC_SYS_APPMGR_ALLOW_LIST_H_

#include <string>
#include <unordered_set>

#include "src/lib/files/unique_fd.h"
#include "src/lib/fxl/macros.h"

namespace component {

// Represents a list of component URLs that are allowed to use a certain feature.
class AllowList {
 public:
  enum Expectation {
    kExpected,
    kOptional,
  };

  // Parses the given file as an allowlist.
  //
  // The file should consist of Component URLs, one per line.
  // No validation is done on the format of the file.
  explicit AllowList(const fxl::UniqueFD& dir, const std::string& file_path, Expectation expected);

  bool IsAllowed(const std::string& url) const {
    return allow_all_ || internal_set_.find(url) != internal_set_.end();
  }

  bool WasFilePresent() const { return file_found_; }

 private:
  std::unordered_set<std::string> internal_set_;
  bool allow_all_;
  bool file_found_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AllowList);
};

}  // namespace component

#endif  // SRC_SYS_APPMGR_ALLOW_LIST_H_
