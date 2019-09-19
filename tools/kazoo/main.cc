// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <cmdline/args_parser.h>

#include "src/lib/files/file.h"
#include "src/lib/fxl/logging.h"
#include "tools/kazoo/outputs.h"
#include "tools/kazoo/syscall_library.h"

namespace {

struct CommandLineOptions {
  std::optional<std::string> arm_asm;
  std::optional<std::string> category;
  std::optional<std::string> kernel_branches;
  std::optional<std::string> kernel_header;
  std::optional<std::string> ktrace;
  std::optional<std::string> syscall_numbers;
  std::optional<std::string> user_header;
  std::optional<std::string> vdso_header;
  std::optional<std::string> vdso_wrappers;
  std::optional<std::string> x86_asm;
};

constexpr const char kHelpIntro[] = R"(kazoo [ <options> ] <fidlc-ir.json>

  kazoo converts from fidlc's json IR representation of syscalls to a variety
  output formats used by the kernel and userspace.

Options:

)";

constexpr const char kArmAsmHelp[] = R"(  --arm-asm=FILENAME
    The output name for the .S file ARM syscalls.)";

constexpr const char kCategoryHelp[] = R"(  --category=FILENAME
    The output name for the .inc categories file.)";

constexpr const char kKernelBranchesHelp[] = R"(  --kernel-branches=FILENAME
    The output name for the .S file used for kernel syscall dispatch.)";

constexpr const char kKernelHeaderHelp[] = R"(  --kernel-header=FILENAME
    The output name for the .h file used for kernel header.)";

constexpr const char kKtraceHelp[] = R"(  --ktrace=FILENAME
    The output name for the .inc file used for kernel tracing.)";

constexpr const char kSyscallNumbersHelp[] = R"(  --syscall-numbers=FILENAME
    The output name for the .h file used for syscall numbers.)";

constexpr const char kUserHeaderHelp[] = R"(  --user-header=FILENAME
    The output name for the .h file used for the user syscall header.)";

constexpr const char kVdsoHeaderHelp[] = R"(  --vdso-header=FILENAME
    The output name for the .h file used for VDSO prototypes.)";

constexpr const char kVdsoWrappersHelp[] = R"(  --vdso-wrappers=FILENAME
    The output name for the .inc file used for blocking VDSO call wrappers.)";

constexpr const char kX86AsmHelp[] = R"(  --x86-asm=FILENAME
    The output name for the .S file x86-64 syscalls.)";

const char kHelpHelp[] = R"(  --help
  -h
    Prints all command line switches.)";

cmdline::Status ParseCommandLine(int argc, const char* argv[], CommandLineOptions* options,
                                 std::vector<std::string>* params) {
  cmdline::ArgsParser<CommandLineOptions> parser;
  parser.AddSwitch("arm-asm", 0, kArmAsmHelp, &CommandLineOptions::arm_asm);
  parser.AddSwitch("category", 0, kCategoryHelp, &CommandLineOptions::category);
  parser.AddSwitch("kernel-branches", 0, kKernelBranchesHelp, &CommandLineOptions::kernel_branches);
  parser.AddSwitch("kernel-header", 0, kKernelHeaderHelp, &CommandLineOptions::kernel_header);
  parser.AddSwitch("ktrace", 0, kKtraceHelp, &CommandLineOptions::ktrace);
  parser.AddSwitch("syscall-numbers", 0, kSyscallNumbersHelp, &CommandLineOptions::syscall_numbers);
  parser.AddSwitch("user-header", 0, kUserHeaderHelp, &CommandLineOptions::user_header);
  parser.AddSwitch("vdso-header", 0, kVdsoHeaderHelp, &CommandLineOptions::vdso_header);
  parser.AddSwitch("vdso-wrappers", 0, kVdsoWrappersHelp, &CommandLineOptions::vdso_wrappers);
  parser.AddSwitch("x86-asm", 0, kX86AsmHelp, &CommandLineOptions::x86_asm);
  bool requested_help = false;
  parser.AddGeneralSwitch("help", 'h', kHelpHelp, [&requested_help]() { requested_help = true; });

  cmdline::Status status = parser.Parse(argc, argv, options, params);
  if (status.has_error()) {
    return status;
  }

  if (requested_help || params->size() != 1) {
    return cmdline::Status::Error(kHelpIntro + parser.GetHelp());
  }

  return cmdline::Status::Ok();
}

}  // namespace

int main(int argc, const char* argv[]) {
  CommandLineOptions options;
  std::vector<std::string> params;
  cmdline::Status status = ParseCommandLine(argc, argv, &options, &params);
  if (status.has_error()) {
    puts(status.error_message().c_str());
    return 1;
  }

  std::string contents;
  if (!files::ReadFileToString(params[0], &contents)) {
    FXL_LOG(ERROR) << "Couldn't read " << params[0] << ".";
    return 1;
  }

  SyscallLibrary library;
  if (!SyscallLibraryLoader::FromJson(contents, &library, /*match_original_order=*/true)) {
    return 1;
  }

  int output_count = 0;

  struct {
    std::optional<std::string>* name;
    bool (*output)(const SyscallLibrary&, Writer*);
  } backends[] = {
      {&options.arm_asm, AsmOutput},
      {&options.category, CategoryOutput},
      {&options.kernel_branches, KernelBranchesOutput},
      {&options.kernel_header, KernelHeaderOutput},
      {&options.ktrace, KtraceOutput},
      {&options.syscall_numbers, SyscallNumbersOutput},
      {&options.user_header, UserHeaderOutput},
      {&options.vdso_header, VdsoHeaderOutput},
      {&options.vdso_wrappers, VdsoWrappersOutput},
      {&options.x86_asm, AsmOutput},
  };

  for (const auto& backend : backends) {
    if (*backend.name) {
      FileWriter writer;
      if (!writer.Open(**backend.name) || !backend.output(library, &writer)) {
        return 1;
      }
      printf("Wrote %s\n", (**backend.name).c_str());
      ++output_count;
    }
  }

  if (output_count == 0) {
    FXL_LOG(WARNING) << "No output types selected.";
    return 1;
  }

  return 0;
}
