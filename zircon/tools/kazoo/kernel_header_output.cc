// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/kazoo/output_util.h"
#include "tools/kazoo/outputs.h"

namespace {

void KernelDeclaration(const Syscall& syscall, Writer* writer) {
  writer->Printf("%s ", GetCKernelModeName(syscall.kernel_return_type()).c_str());
  writer->Printf("sys_%s(\n", syscall.name().c_str());

  if (syscall.kernel_arguments().size() == 0) {
    // TODO(syscall-fidl-transition): Drop this, and maybe the preceding \n.
    writer->Printf("    ");
  } else {
    for (size_t i = 0; i < syscall.kernel_arguments().size(); ++i) {
      const StructMember& arg = syscall.kernel_arguments()[i];
      const bool last = i == syscall.kernel_arguments().size() - 1;
      writer->Printf("    %s %s%s", GetCKernelModeName(arg.type()).c_str(), arg.name().c_str(),
                     last ? "" : ",\n");
    }
  }
  writer->Printf(")");
  if (syscall.HasAttribute("Noreturn")) {
    writer->Printf(" __NO_RETURN");
  }
  writer->Printf(";\n\n");
}

} // namespace

bool KernelHeaderOutput(const SyscallLibrary& library, Writer* writer) {
  if (!CopyrightHeaderWithCppComments(writer)) {
    return false;
  }

  for (const auto& syscall : library.syscalls()) {
    if (syscall->HasAttribute("Vdsocall")) {
      continue;
    }

    KernelDeclaration(*syscall, writer);
  }

  // TODO(syscall-fidl-transition): Original file has an extra \n, add one here
  // for consistency.
  writer->Puts("\n");

  return true;
}
