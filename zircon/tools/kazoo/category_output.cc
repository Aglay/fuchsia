// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/kazoo/output_util.h"
#include "tools/kazoo/outputs.h"

bool CategoryOutput(const SyscallLibrary& library, Writer* writer) {
  if (!CopyrightHeaderWithCppComments(writer)){
    return false;
  }

  const char* kCategories[] = {
      "Blocking", "Const", "Noreturn", "TestCategory1", "TestCategory2", "Vdsocall",
  };

  for (const char* category : kCategories) {
    std::vector<std::string> syscalls_in_category;
    for (const auto& syscall : library.syscalls()) {
      if (syscall->HasAttribute(category)) {
        syscalls_in_category.push_back(syscall->name());
      }
    }

    if (!syscalls_in_category.empty()) {
      std::string category_kernel_style = CamelToSnake(category);
      // TODO(syscall-fidl-transition): Extra leading \n here for consistency.
      writer->Printf("\n#define HAVE_SYSCALL_CATEGORY_%s 1\n", category_kernel_style.c_str());
      writer->Printf("SYSCALL_CATEGORY_BEGIN(%s)\n", category_kernel_style.c_str());
      for (const auto& name : syscalls_in_category) {
        writer->Printf("    SYSCALL_IN_CATEGORY(%s)\n", name.c_str());
      }
      writer->Printf("SYSCALL_CATEGORY_END(%s)\n", category_kernel_style.c_str());
    }
  }

  return true;
}
