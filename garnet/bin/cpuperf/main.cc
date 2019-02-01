// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

#include <lib/fxl/command_line.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/log_settings.h>
#include <lib/fxl/log_settings_command_line.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/strings/split_string.h>

#ifdef __x86_64__  // For non-supported arches we're just a stub.

#include <lib/zircon-internal/device/cpu-trace/cpu-perf.h>
#include <zircon/syscalls.h>

#include "garnet/lib/cpuperf/controller.h"
#include "garnet/lib/cpuperf/events.h"
#include "garnet/lib/debugger_utils/util.h"

#include "print_tallies.h"
#include "session_spec.h"
#include "session_result_spec.h"

const char kUsageString[] =
    "Usage: cpuperf [options]\n"
    "\n"
    "Options:\n"
    "  --spec-file=FILE   Use the cpuperf specification data in FILE\n"
    "  --help             Show this help message and exit\n"
    "  --list-events      Print the list of supported events\n"
    "  --describe-event=EVENT  Print a description of EVENT\n"
    "                     Event is specified as group:name\n"
    "\n"
    "Logging options:\n"
    "  --quiet[=LEVEL]    Set quietness level (opposite of verbose)\n"
    "  --verbose[=LEVEL]  Set debug verbosity level\n"
    "  --log-file=FILE    Write log output to FILE.\n"
    "Quiet supersedes verbose if both are specified.\n"
    "Defined log levels:\n"
    "-n - verbosity level n\n"
    " 0 - INFO - this is the default level\n"
    " 1 - WARNING\n"
    " 2 - ERROR\n"
    " 3 - FATAL\n";

static void PrintUsageString(FILE* f) {
  fputs(kUsageString, f);
}

static bool GetSessionSpecFromArgv(const fxl::CommandLine& cl,
                                   cpuperf::SessionSpec* out_spec) {
  std::string arg;

  if (cl.GetOptionValue("spec-file", &arg)) {
    std::string content;
    if (!files::ReadFileToString(arg, &content)) {
      FXL_LOG(ERROR) << "Can't read " << arg;
      return false;
    }
    if (!cpuperf::DecodeSessionSpec(content, out_spec)) {
      return false;
    }
  }

  return true;
}

static void DescribeEvent(FILE* f, const cpuperf::EventDetails* details) {
  if (details->description[0] != '\0') {
    fprintf(f, "%s: %s\n", details->name, details->description);
  } else {
    // Print some kind of description for consistency.
    // The output in some sessions (e.g., emacs) gets colorized due to
    // the presence of colons, and it's harder to read without consistency.
    // Printing "missing description" will help encourage adding one. :-)
    fprintf(f, "%s: <missing description>\n", details->name);
  }
}

static void DescribeEvent(FILE* f, const std::string& full_name) {
  std::vector<std::string> parts =
    fxl::SplitStringCopy(full_name, ":", fxl::kTrimWhitespace,
                         fxl::kSplitWantAll);
  if (parts.size() != 2) {
    FXL_LOG(ERROR) << "Usage: cpuperf --describe-event=group:name";
    exit(EXIT_FAILURE);
  }

  const cpuperf::EventDetails* details;
  if (!LookupEventByName(parts[0].c_str(), parts[1].c_str(),
                         &details)) {
    FXL_LOG(ERROR) << "Unknown event: " << full_name;
    exit(EXIT_FAILURE);
  }

  DescribeEvent(f, details);
}

static void PrintEventList(FILE* f) {
  cpuperf::GroupTable groups = cpuperf::GetAllGroups();

  for (auto& group : groups) {
    std::sort(group.events.begin(), group.events.end(),
              [] (const cpuperf::EventDetails*& a,
                  const cpuperf::EventDetails*& b) {
      return strcmp(a->name, b->name) < 0;
    });
    fprintf(f, "\nGroup %s\n", group.group_name);
    for (const auto& event : group.events) {
      DescribeEvent(f, event);
    }
  }
}

static void SaveTrace(const cpuperf::SessionResultSpec& result_spec,
                      cpuperf::Controller* controller, size_t iter) {
  std::unique_ptr<cpuperf::DeviceReader> reader = controller->GetReader();
  if (!reader) {
    return;
  }

  FXL_VLOG(1) << "Saving results of iteration " << iter;

  for (size_t trace = 0; trace < result_spec.num_traces; ++trace) {
    if (reader->SetTrace(trace) != cpuperf::ReaderStatus::kOk) {
      // If we can't set the trace to this one it's unlikely we can continue.
      return;
    }

    auto buffer = reinterpret_cast<const char*>(
        reader->GetCurrentTraceBuffer());
    FXL_DCHECK(buffer);
    size_t size = reader->GetCurrentTraceSize();
    FXL_DCHECK(size > 0);
    std::string output_file_path = result_spec.GetTraceFilePath(iter, trace);
    if (!files::WriteFile(output_file_path, buffer, size)) {
      FXL_LOG(ERROR) << "Error saving trace data to: " << output_file_path;
      // If writing this one fails, it's unlikely we can continue.
      return;
    }
  }

  // Print a summary of this run.
  // In tally mode this is noise, but if verbosity is on sure.
  FXL_LOG(INFO) << "Iteration " << iter << " summary";
  if (controller->mode() != cpuperf::Controller::Mode::kTally ||
      FXL_VLOG_IS_ON(2)) {
    for (size_t trace = 0; trace < result_spec.num_traces; ++trace) {
      std::string path = result_spec.GetTraceFilePath(iter, trace);
      uint64_t size;
      if (files::GetFileSize(path, &size)) {
        FXL_LOG(INFO) << path << ": " << size;
      } else {
        FXL_LOG(INFO) << path << ": unknown size";
      }
    }
  }
}

static bool RunSession(const cpuperf::SessionSpec& spec,
                       cpuperf::Controller* controller) {
  cpuperf::SessionResultSpec result_spec{spec.config_name, spec.num_iterations,
      controller->num_traces(), spec.output_path_prefix};

  for (size_t iter = 0; iter < spec.num_iterations; ++iter) {
    if (!controller->Start()) {
      return false;
    }

    zx::nanosleep(zx::deadline_after(zx::duration(
        spec.duration.ToNanoseconds())));

    controller->Stop();

    if (controller->mode() == cpuperf::Controller::Mode::kTally) {
      PrintTallyResults(stdout, spec, result_spec, controller);
    } else {
      SaveTrace(result_spec, controller, iter);
    }
  }

  if (controller->mode() != cpuperf::Controller::Mode::kTally) {
    if (!cpuperf::WriteSessionResultSpec(spec.session_result_spec_path,
                                         result_spec)) {
      return false;
    }
  }

  return true;
}

int main(int argc, char* argv[]) {
  fxl::CommandLine cl = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(cl))
    return EXIT_FAILURE;

  if (cl.HasOption("help", nullptr)) {
    PrintUsageString(stdout);
    return EXIT_SUCCESS;
  }

  if (cl.HasOption("list-events", nullptr)) {
    PrintEventList(stdout);
    return EXIT_SUCCESS;
  }

  std::string arg;
  if (cl.GetOptionValue("describe-event", &arg)) {
    DescribeEvent(stdout, arg);
    return EXIT_SUCCESS;
  }

  // TODO(dje): dump-arch option
  // TODO(dje): Command line options for parts of the spec.

  cpuperf::SessionSpec spec;
  if (!GetSessionSpecFromArgv(cl, &spec)) {
    return EXIT_FAILURE;
  }

  if (cpuperf::GetConfigEventCount(spec.cpuperf_config) == 0) {
    FXL_LOG(ERROR) << "No events specified";
    return EXIT_FAILURE;
  }

  std::unique_ptr<cpuperf::Controller> controller;
  if (!cpuperf::Controller::Create(spec.buffer_size_in_mb,
                                   spec.cpuperf_config,
                                   &controller)) {
    return EXIT_FAILURE;
  }

  FXL_LOG(INFO) << "cpuperf control program starting";
  FXL_LOG(INFO) << spec.num_iterations << " iteration(s), "
                << spec.duration.ToSeconds() << " second(s) per iteration";

  bool success = RunSession(spec, controller.get());

  if (!success) {
    FXL_LOG(INFO) << "cpuperf exiting with error";
    return EXIT_FAILURE;
  }

  FXL_LOG(INFO) << "cpuperf control program exiting";
  return EXIT_SUCCESS;
}

#else  // !__x86_64__

int main(int argc, char* argv[]) {
  FXL_LOG(ERROR) << "cpuperf is currently for x86_64 only";
  return EXIT_FAILURE;
}

#endif  // !__x86_64__
