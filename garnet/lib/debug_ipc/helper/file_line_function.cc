// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/debug_ipc/helper/file_line_function.h"

#include "lib/fxl/strings/string_printf.h"

namespace debug_ipc {

FileLineFunction::FileLineFunction() = default;
FileLineFunction::FileLineFunction(std::string file, int line,
                                   std::string function)
    : file_(std::move(file)), function_(std::move(function)), line_(line) {}
FileLineFunction::~FileLineFunction() = default;

bool operator<(const FileLineFunction& a, const FileLineFunction& b) {
  if (a.line() != b.line())
    return a.line() < b.line();
  return a.file() < b.file();
}

std::string FileLineFunction::ToString() const {
  if (function_.empty()) {
    return fxl::StringPrintf("%s:%d", file_.data(), line_);
  } else {
    return fxl::StringPrintf("%s:%d (%s)", file_.data(), line_,
                             function_.data());
  }
}

bool operator==(const FileLineFunction& a, const FileLineFunction& b) {
  return a.line() == b.line() && a.file() == b.file();
}

bool operator!=(const FileLineFunction& a, const FileLineFunction& b) {
  return !operator==(a, b);
}

}  // namespace debug_ipc
