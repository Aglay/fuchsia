// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "args.h"

#include <lib/cmdline/args_parser.h>
#include <lib/fitx/result.h>
#include <stdlib.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/lib/fxl/strings/string_printf.h"

namespace hwstress {
namespace {

std::unique_ptr<cmdline::ArgsParser<CommandLineArgs>> GetParser() {
  auto parser = std::make_unique<cmdline::ArgsParser<CommandLineArgs>>();
  parser->AddSwitch("duration", 'd', "Test duration in seconds.",
                    &CommandLineArgs::test_duration_seconds);
  parser->AddSwitch("fvm-path", 'f', "Path to Fuchsia Volume Manager.", &CommandLineArgs::fvm_path);
  parser->AddSwitch("help", 'h', "Show this help.", &CommandLineArgs::help);
  parser->AddSwitch("verbose", 'v', "Show verbose logging.", &CommandLineArgs::verbose);
  parser->AddSwitch("memory", 'm', "Amount of memory to test in megabytes.",
                    &CommandLineArgs::mem_to_test_megabytes);
  parser->AddSwitch("percent-memory", 0, "Percent of memory to test.",
                    &CommandLineArgs::ram_to_test_percent);
  parser->AddSwitch("utilization", 'u', "Target CPU utilization percent.",
                    &CommandLineArgs::utilization_percent);
  return parser;
}

}  // namespace

std::istream& operator>>(std::istream& is, OptionalInt64& result) {
  is >> result.emplace();
  return is;
}

void PrintUsage() {
  puts(
      R"(usage:
hwstress <subcommand> [options]

Attempts to stress hardware components by placing them under high load.

Subcommands:
  cpu                    Perform a CPU stress test.
  flash                  Perform a flash stress test.
  light                  Perform a device light / LED stress test.
  memory                 Perform a RAM stress test.

Global options:
  -d, --duration=<secs>  Test duration in seconds. A value of "0" (the default)
                         indicates to continue testing until stopped.
  -v, --verbose          Show additional logging.
  -h, --help             Show this help.

CPU test options:
  -u, --utilization=<percent>
                         Percent of system CPU to use. A value of
                         100 (the default) indicates that all the
                         CPU should be used, while 50 would indicate
                         to use 50% of CPU. Must be strictly greater
                         than 0, and no more than 100.

Flash test options:
  -f, --fvm-path=<path>  Path to Fuchsia Volume Manager
  -m, --memory=<size>    Amount of flash memory to test, in megabytes.

Memory test options:
  -m, --memory=<size>    Amount of RAM to test, in megabytes.
  --percent-memory=<percent>
                         Percentage of total system RAM to test.
)");
}

fitx::result<std::string, CommandLineArgs> ParseArgs(fbl::Span<const char* const> args) {
  CommandLineArgs result;
  StressTest subcommand;

  // Ensure a subcommand was provided.
  if (args.size() < 2) {
    return fitx::error("A subcommand specifying what type of test to run must be specified.");
  }
  std::string_view first_arg(args.data()[1]);

  // If "--help" or "-h" was provided, don't try parsing anything else.
  if (first_arg == "-h" || first_arg == "--help") {
    result.help = true;
    return fitx::success(result);
  }

  // Parse the subcommand.
  if (first_arg == std::string_view("cpu")) {
    subcommand = StressTest::kCpu;
  } else if (first_arg == std::string_view("flash")) {
    subcommand = StressTest::kFlash;
  } else if (first_arg == std::string_view("memory")) {
    subcommand = StressTest::kMemory;
  } else if (first_arg == std::string_view("light")) {
    subcommand = StressTest::kLight;
  } else {
    return fitx::error(
        fxl::StringPrintf("Unknown subcommand or option: '%s'.", std::string(first_arg).data())
            .c_str());
  }

  fbl::Span other_args = args.subspan(1);  // Strip first element.

  std::unique_ptr<cmdline::ArgsParser<CommandLineArgs>> parser = GetParser();
  std::vector<std::string> params;
  cmdline::Status status = parser->Parse(other_args.size(), other_args.data(), &result, &params);
  if (!status.ok()) {
    return fitx::error(status.error_message().c_str());
  }

  // If help is provided, ignore any further invalid args and just show the
  // help screen.
  if (result.help) {
    return fitx::success(result);
  }

  result.subcommand = subcommand;

  // Validate duration.
  if (result.test_duration_seconds < 0) {
    return fitx::error("Test duration cannot be negative.");
  }

  // Validate memory flags.
  if (result.ram_to_test_percent.has_value()) {
    if (result.ram_to_test_percent.value() <= 0 || result.ram_to_test_percent.value() >= 100) {
      return fitx::error("Percent of RAM to test must be between 1 and 99, inclusive.");
    }
  }
  if (result.mem_to_test_megabytes.has_value()) {
    if (result.mem_to_test_megabytes.value() <= 0) {
      return fitx::error("RAM to test must be strictly positive.");
    }
  }
  if (result.mem_to_test_megabytes.has_value() && result.ram_to_test_percent.has_value()) {
    return fitx::error("--memory and --percent-memory cannot both be specified.");
  }

  // Validate utilization.
  if (result.utilization_percent <= 0.0 || result.utilization_percent > 100.0) {
    return fitx::error("--utilization must be greater than 0%%, and no more than 100%%.");
  }

  // Ensure mandatory flash test argument is provided
  if (result.subcommand == StressTest::kFlash && result.fvm_path.empty()) {
    return fitx::error(fxl::StringPrintf("Path to Fuchsia Volume Manager must be specified"));
  }

  // Ensure no more parameters were given.
  if (!params.empty()) {
    return fitx::error(fxl::StringPrintf("Unknown option: '%s'.", params[1].c_str()).c_str());
  }

  return fitx::success(result);
}

}  // namespace hwstress
