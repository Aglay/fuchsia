// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fstream>
#include <iostream>

#include "apps/tracing/src/trace/commands/record.h"
#include "apps/tracing/src/trace/spec.h"
#include "lib/ftl/files/file.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/split_string.h"
#include "lib/ftl/strings/string_number_conversions.h"
#include "lib/mtl/tasks/message_loop.h"

namespace tracing {
namespace {

std::ostream& operator<<(std::ostream& os, measure::DurationSpec spec) {
  return os << "duration of " << spec.event;
}

std::ostream& operator<<(std::ostream& os, measure::TimeBetweenSpec spec) {
  return os << "time between " << spec.first_event << "and "
            << spec.second_event;
}

// This just prints the results out verbatim as ticks. TODO(ppi): calculate
// useful human-readable statistics.
template <typename Spec>
void PrintResults(
    std::ostream& out,
    const Spec& spec,
    const std::unordered_map<uint64_t, std::vector<Ticks>>& results) {
  out << spec << " : ";
  if (!results.count(spec.id)) {
    out << " no results" << std::endl;
    return;
  }

  const std::vector<Ticks>& ticks = results.at(spec.id);
  out << "[";
  for (size_t i = 0u; i < ticks.size(); i++) {
    if (i != 0) {
      out << ", ";
    }
    out << ticks[i];
  }
  out << "]";
  out << std::endl;
}

}  // namespace

bool Record::Options::Setup(const ftl::CommandLine& command_line) {
  size_t index = 0;
  // Read the spec file first. Arguments passed on the command line override the
  // spec.
  // --spec-file=<file>
  if (command_line.HasOption("spec-file", &index)) {
    std::string spec_file_path = command_line.options()[index].value;
    if (!files::IsFile(spec_file_path)) {
      err() << spec_file_path << " is not a file" << std::endl;
      return false;
    }

    std::string content;
    if (!files::ReadFileToString(spec_file_path, &content)) {
      err() << "Can't read " << spec_file_path << std::endl;
      return false;
    }

    Spec spec;
    if (!DecodeSpec(content, &spec)) {
      err() << "Can't decode " << spec_file_path << std::endl;
      return false;
    }
    app = std::move(spec.app);
    args = std::move(spec.args);
    categories = std::move(spec.categories);
    duration = std::move(spec.duration);
    measure_duration_specs = std::move(spec.duration_specs);
    measure_time_between_specs = std::move(spec.time_between_specs);
  }

  // --categories=<cat1>,<cat2>,...
  if (command_line.HasOption("categories", &index)) {
    categories =
        ftl::SplitStringCopy(command_line.options()[index].value, ",",
                             ftl::kTrimWhitespace, ftl::kSplitWantNonEmpty);
  }

  // --output-file=<file>
  if (command_line.HasOption("output-file", &index)) {
    output_file_name = command_line.options()[index].value;
  }

  // --duration=<seconds>
  if (command_line.HasOption("duration", &index)) {
    uint64_t seconds;
    if (!ftl::StringToNumberWithError(command_line.options()[index].value,
                                      &seconds)) {
      FTL_LOG(ERROR) << "Failed to parse command-line option duration: "
                     << command_line.options()[index].value;
      return false;
    }
    duration = ftl::TimeDelta::FromSeconds(seconds);
  }

  // --detach
  detach = command_line.HasOption("detach");

  // --decouple
  decouple = command_line.HasOption("decouple");

  // --buffer-size=<megabytes>
  if (command_line.HasOption("buffer-size", &index)) {
    uint32_t megabytes;
    if (!ftl::StringToNumberWithError(command_line.options()[index].value,
                                      &megabytes)) {
      FTL_LOG(ERROR) << "Failed to parse command-line option buffer-size: "
                     << command_line.options()[index].value;
      return false;
    }
    buffer_size_megabytes_hint = megabytes;
  }

  // <command> <args...>
  const auto& positional_args = command_line.positional_args();
  if (!positional_args.empty()) {
    if (!app.empty() || !args.empty()) {
      FTL_LOG(WARNING) << "The app and args passed on the command line"
                       << "override those from the tspec file.";
    }
    app = positional_args[0];
    args = std::vector<std::string>(positional_args.begin() + 1,
                                    positional_args.end());
  }

  return true;
}

Command::Info Record::Describe() {
  return Command::Info{
      [](app::ApplicationContext* context) {
        return std::make_unique<Record>(context);
      },
      "record",
      "starts tracing and records data",
      {{"spec-file=[none]", "Tracing specification file"},
       {"output-file=[/tmp/trace.json]", "Trace data is stored in this file"},
       {"duration=[10s]",
        "Trace will be active for this long after the session has been "
        "started"},
       {"categories=[\"\"]", "Categories that should be enabled for tracing"},
       {"detach=[false]",
        "Don't stop the traced program when tracing finished"},
       {"decouple=[false]", "Don't stop tracing when the traced program exits"},
       {"buffer-size=[4]",
        "Maximum size of trace buffer for each provider in megabytes"},
       {"[command args]",
        "Run program before starting trace. The program is terminated when "
        "tracing ends unless --detach is specified"}}};
}

Record::Record(app::ApplicationContext* context)
    : CommandWithTraceController(context), weak_ptr_factory_(this) {}

void Record::Run(const ftl::CommandLine& command_line) {
  if (!options_.Setup(command_line)) {
    err() << "Error parsing options from command line - aborting" << std::endl;
    exit(1);
  }

  std::ofstream out_file(options_.output_file_name,
                         std::ios_base::out | std::ios_base::trunc);
  if (!out_file.is_open()) {
    err() << "Failed to open " << options_.output_file_name << " for writing"
          << std::endl;
    exit(1);
  }

  exporter_.reset(new ChromiumExporter(std::move(out_file)));
  tracer_.reset(new Tracer(trace_controller().get()));
  if (!options_.measure_duration_specs.empty()) {
    aggregate_events_ = true;
    measure_duration_.reset(
        new measure::MeasureDuration(options_.measure_duration_specs));
  }
  if (!options_.measure_time_between_specs.empty()) {
    aggregate_events_ = true;
    measure_time_between_.reset(
        new measure::MeasureTimeBetween(options_.measure_time_between_specs));
  }

  tracing_ = true;

  auto trace_options = TraceOptions::New();
  trace_options->categories =
      fidl::Array<fidl::String>::From(options_.categories);
  trace_options->buffer_size_megabytes_hint =
      options_.buffer_size_megabytes_hint;

  tracer_->Start(
      std::move(trace_options),
      [this](const reader::Record& record) {
        exporter_->ExportRecord(record);

        if (aggregate_events_ && record.type() == RecordType::kEvent) {
          events_.push_back(record.GetEvent());
        }
      },
      [](std::string error) { err() << error; },
      [this] {
        if (!options_.app.empty())
          LaunchApp();
        StartTimer();
      },
      [this] { DoneTrace(); });
}

void Record::StopTrace() {
  if (tracing_) {
    out() << "Stopping trace..." << std::endl;
    tracing_ = false;
    tracer_->Stop();
  }
}

void Record::DoneTrace() {
  tracer_.reset();
  exporter_.reset();

  out() << "Trace file written to " << options_.output_file_name << std::endl;

  if (!events_.empty()) {
    std::sort(
        std::begin(events_), std::end(events_),
        [](const reader::Record::Event& e1, const reader::Record::Event& e2) {
          return e1.timestamp < e2.timestamp;
        });
  }

  for (const auto& event : events_) {
    if (measure_duration_) {
      measure_duration_->Process(event);
    }
    if (measure_time_between_) {
      measure_time_between_->Process(event);
    }
  }

  for (auto& spec : options_.measure_duration_specs) {
    PrintResults(out(), spec, measure_duration_->results());
  }

  for (auto& spec : options_.measure_time_between_specs) {
    PrintResults(out(), spec, measure_time_between_->results());
  }

  mtl::MessageLoop::GetCurrent()->QuitNow();
}

void Record::LaunchApp() {
  auto launch_info = app::ApplicationLaunchInfo::New();
  launch_info->url = fidl::String::From(options_.app);
  launch_info->arguments = fidl::Array<fidl::String>::From(options_.args);

  out() << "Launching " << launch_info->url;
  context()->launcher()->CreateApplication(std::move(launch_info),
                                           GetProxy(&application_controller_));
  application_controller_.set_connection_error_handler([this] {
    out() << "Application terminated";
    if (!options_.decouple)
      StopTrace();
  });
  if (options_.detach)
    application_controller_->Detach();
}

void Record::StartTimer() {
  mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [weak = weak_ptr_factory_.GetWeakPtr()] {
        if (weak)
          weak->StopTrace();
      },
      options_.duration);
  out() << "Starting trace; will stop in " << options_.duration.ToSecondsF()
        << " seconds..." << std::endl;
}

}  // namespace tracing
