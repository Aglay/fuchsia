// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/pkg_url/fuchsia_pkg_url.h"
#include "lib/fxl/strings/concatenate.h"
#include "lib/fxl/strings/substitute.h"

#include <regex>
#include <string>

namespace component {

constexpr char kFuchsiaPkgPrefix[] = "fuchsia-pkg://";
// Assume anything between the last / and # is the package name.
// TODO(CP-110): Support pkg-variant and pkg-hash.
static const std::regex* const kPackageName = new std::regex("([^/]+)(?=#|$)");
// Resource path is anything after #.
static const std::regex* const kHasResource = new std::regex("#");
static const std::regex* const kResourcePath = new std::regex("([^#]+)$");

// static
bool FuchsiaPkgUrl::IsFuchsiaPkgScheme(const std::string& url) {
  return url.find(kFuchsiaPkgPrefix) == 0;
}

std::string FuchsiaPkgUrl::GetDefaultComponentCmxPath() const {
  return fxl::Substitute("meta/$0.cmx", package_name());
}

std::string FuchsiaPkgUrl::GetDefaultComponentName() const {
  return package_name();
}

bool FuchsiaPkgUrl::Parse(const std::string& url) {
  package_name_.clear();
  resource_path_.clear();

  if (!IsFuchsiaPkgScheme(url)) {
    return false;
  }

  url_ = url;
  std::smatch sm;
  if (!(std::regex_search(url, sm, *kPackageName) && sm.size() >= 2)) {
    return false;
  }
  package_name_ = sm[1].str();
  if (std::regex_search(url, sm, *kHasResource)) {
    if (!(std::regex_search(url, sm, *kResourcePath) && sm.size() >= 2)) {
      return false;
    }
    resource_path_ = sm[1].str();
  }
  return true;
}

std::string FuchsiaPkgUrl::pkgfs_dir_path() const {
  // TODO(CP-105): We're currently hardcoding version 0 of the package,
  // but we'll eventually need to do something smarter.
  return fxl::Concatenate({"/pkgfs/packages/", package_name(), "/0"});
}

const std::string& FuchsiaPkgUrl::ToString() const { return url_; }

}  // namespace component
