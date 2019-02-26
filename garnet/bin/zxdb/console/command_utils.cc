// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/console/command_utils.h"

#include <inttypes.h>
#include <stdio.h>
#include <limits>

#include "garnet/bin/zxdb/client/breakpoint.h"
#include "garnet/bin/zxdb/client/frame.h"
#include "garnet/bin/zxdb/client/job.h"
#include "garnet/bin/zxdb/client/job_context.h"
#include "garnet/bin/zxdb/client/process.h"
#include "garnet/bin/zxdb/client/target.h"
#include "garnet/bin/zxdb/client/thread.h"
#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/console/command.h"
#include "garnet/bin/zxdb/console/console_context.h"
#include "garnet/bin/zxdb/console/output_buffer.h"
#include "garnet/bin/zxdb/console/string_util.h"
#include "garnet/bin/zxdb/expr/identifier.h"
#include "garnet/bin/zxdb/symbols/function.h"
#include "garnet/bin/zxdb/symbols/location.h"
#include "garnet/bin/zxdb/symbols/symbol_utils.h"
#include "garnet/bin/zxdb/symbols/variable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/fxl/strings/trim.h"

namespace zxdb {

Err AssertRunningTarget(ConsoleContext* context, const char* command_name,
                        Target* target) {
  Target::State state = target->GetState();
  if (state == Target::State::kRunning)
    return Err();
  return Err(
      ErrType::kInput,
      fxl::StringPrintf("%s requires a running process but process %d is %s.",
                        command_name, context->IdForTarget(target),
                        TargetStateToString(state).c_str()));
}

Err AssertStoppedThreadCommand(ConsoleContext* context, const Command& cmd,
                               bool validate_nouns, const char* command_name) {
  Err err;
  if (validate_nouns) {
    err = cmd.ValidateNouns({Noun::kProcess, Noun::kThread});
    if (err.has_error())
      return err;
  }

  if (!cmd.thread()) {
    return Err(fxl::StringPrintf(
        "\"%s\" requires a thread but there is no current thread.",
        command_name));
  }
  if (cmd.thread()->GetState() != debug_ipc::ThreadRecord::State::kBlocked &&
      cmd.thread()->GetState() != debug_ipc::ThreadRecord::State::kCoreDump &&
      cmd.thread()->GetState() != debug_ipc::ThreadRecord::State::kSuspended) {
    return Err(fxl::StringPrintf(
        "\"%s\" requires a suspended thread but thread %d is %s.\n"
        "To view and sync thread state with the remote system, type "
        "\"thread\".",
        command_name, context->IdForThread(cmd.thread()),
        ThreadStateToString(cmd.thread()->GetState(),
                            cmd.thread()->GetBlockedReason())
            .c_str()));
  }
  return Err();
}

size_t CheckHexPrefix(const std::string& s) {
  if (s.size() >= 2u && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    return 2u;
  return 0u;
}

Err StringToInt(const std::string& s, int* out) {
  if (s.empty())
    return Err(ErrType::kInput, "The empty string is not a number.");

  std::string trimmed = fxl::TrimString(s, " ").ToString();

  // Re-uses StringToUint64's error handling and just adds support for '-' at
  // the beginning and size-checks the output.
  uint64_t absolute_val;
  if (trimmed[0] == '-') {
    Err err = StringToUint64(trimmed.substr(1), &absolute_val);
    if (err.has_error())
      return err;
    if (absolute_val >
        -static_cast<int64_t>(std::numeric_limits<int>::lowest()))
      return Err("This value is too small for an integer.");
    *out = -static_cast<int>(absolute_val);
  } else {
    Err err = StringToUint64(trimmed, &absolute_val);
    if (err.has_error())
      return err;
    if (absolute_val > std::numeric_limits<int>::max())
      return Err("This value is too large for an integer.");
    *out = static_cast<int>(absolute_val);
  }

  return Err();
}

Err StringToUint32(const std::string& s, uint32_t* out) {
  // Re-uses StringToUint64's error handling and just size-checks the output.
  uint64_t value64;
  Err err = StringToUint64(s, &value64);
  if (err.has_error())
    return err;

  if (value64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return Err(fxl::StringPrintf(
        "Expected 32-bit unsigned value, but %s is too large.", s.c_str()));
  }
  *out = static_cast<uint32_t>(value64);
  return Err();
}

Err StringToUint64(const std::string& s, uint64_t* out) {
  std::string trimmed = fxl::TrimString(s, " ").ToString();

  *out = 0;
  if (trimmed.empty())
    return Err(ErrType::kInput, "The empty string is not a number.");

  if (size_t hex_after_prefix = CheckHexPrefix(trimmed)) {
    if (hex_after_prefix == trimmed.size())
      return Err(ErrType::kInput, "Expecting number after \"0x\".");
    for (size_t i = hex_after_prefix; i < trimmed.size(); i++) {
      if (!isxdigit(trimmed[i]))
        return Err(ErrType::kInput,
                   "Invalid hex number: + \"" + trimmed + "\".");
    }
    *out = strtoull(trimmed.c_str(), nullptr, 16);
  } else {
    for (size_t i = 0; i < trimmed.size(); i++) {
      if (!isdigit(trimmed[i]))
        return Err(ErrType::kInput, "Invalid number: \"" + trimmed + "\".");
    }
    *out = strtoull(trimmed.c_str(), nullptr, 10);
  }

  return Err();
}

Err ReadUint64Arg(const Command& cmd, size_t arg_index, const char* param_desc,
                  uint64_t* out) {
  if (cmd.args().size() <= arg_index) {
    return Err(ErrType::kInput,
               fxl::StringPrintf("Not enough arguments when reading the %s.",
                                 param_desc));
  }
  Err result = StringToUint64(cmd.args()[arg_index], out);
  if (result.has_error()) {
    return Err(ErrType::kInput,
               fxl::StringPrintf("Invalid number \"%s\" when reading the %s.",
                                 cmd.args()[arg_index].c_str(), param_desc));
  }
  return Err();
}

Err ParseHostPort(const std::string& in_host, const std::string& in_port,
                  std::string* out_host, uint16_t* out_port) {
  if (in_host.empty())
    return Err(ErrType::kInput, "No host component specified.");
  if (in_port.empty())
    return Err(ErrType::kInput, "No port component specified.");

  // Trim brackets from the host name for IPv6 addresses.
  if (in_host.front() == '[' && in_host.back() == ']')
    *out_host = in_host.substr(1, in_host.size() - 2);
  else
    *out_host = in_host;

  // Re-use paranoid int64 parsing.
  uint64_t port64;
  Err err = StringToUint64(in_port, &port64);
  if (err.has_error())
    return err;
  if (port64 == 0 || port64 > std::numeric_limits<uint16_t>::max())
    return Err(ErrType::kInput, "Port value out of range.");
  *out_port = static_cast<uint16_t>(port64);

  return Err();
}

Err ParseHostPort(const std::string& input, std::string* out_host,
                  uint16_t* out_port) {
  // Separate based on the last colon.
  size_t colon = input.rfind(':');
  if (colon == std::string::npos)
    return Err(ErrType::kInput, "Expected colon to separate host/port.");

  // If the host has a colon in it, it could be an IPv6 address. In this case,
  // require brackets around it to differentiate the case where people
  // supplied an IPv6 address and we just picked out the last component above.
  std::string host = input.substr(0, colon);
  if (host.empty())
    return Err(ErrType::kInput, "No host component specified.");
  if (host.find(':') != std::string::npos) {
    if (host.front() != '[' || host.back() != ']') {
      return Err(ErrType::kInput,
                 "For IPv6 addresses use either: \"[::1]:1234\"\n"
                 "or the two-parameter form: \"::1 1234");
    }
  }

  std::string port = input.substr(colon + 1);

  return ParseHostPort(host, port, out_host, out_port);
}

std::string TargetStateToString(Target::State state) {
  switch (state) {
    case Target::State::kNone:
      return "Not running";
    case Target::State::kStarting:
      return "Starting";
    case Target::State::kAttaching:
      return "Attaching";
    case Target::State::kRunning:
      return "Running";
  }
  FXL_NOTREACHED();
  return std::string();
}

std::string JobContextStateToString(JobContext::State state) {
  switch (state) {
    case JobContext::State::kNone:
      return "Not running";
    case JobContext::State::kStarting:
      return "Starting";
    case JobContext::State::kAttaching:
      return "Attaching";
    case JobContext::State::kRunning:
      return "Running";
  }
  FXL_NOTREACHED();
  return std::string();
}

std::string ThreadStateToString(
    debug_ipc::ThreadRecord::State state,
    debug_ipc::ThreadRecord::BlockedReason blocked_reason) {
  // Blocked can have many cases, so we handle it separatedly.
  if (state != debug_ipc::ThreadRecord::State::kBlocked)
    return debug_ipc::ThreadRecord::StateToString(state);

  FXL_DCHECK(blocked_reason !=
             debug_ipc::ThreadRecord::BlockedReason::kNotBlocked)
      << "A blocked thread has to have a valid reason.";
  return fxl::StringPrintf(
      "Blocked (%s)",
      debug_ipc::ThreadRecord::BlockedReasonToString(blocked_reason));
}

std::string BreakpointScopeToString(const ConsoleContext* context,
                                    const BreakpointSettings& settings) {
  switch (settings.scope) {
    case BreakpointSettings::Scope::kSystem:
      return "Global";
    case BreakpointSettings::Scope::kTarget:
      return fxl::StringPrintf("pr %d",
                               context->IdForTarget(settings.scope_target));
    case BreakpointSettings::Scope::kThread:
      return fxl::StringPrintf(
          "pr %d t %d",
          context->IdForTarget(
              settings.scope_thread->GetProcess()->GetTarget()),
          context->IdForThread(settings.scope_thread));
  }
  FXL_NOTREACHED();
  return std::string();
}

std::string BreakpointStopToString(BreakpointSettings::StopMode mode) {
  switch (mode) {
    case BreakpointSettings::StopMode::kNone:
      return "None";
    case BreakpointSettings::StopMode::kThread:
      return "Thread";
    case BreakpointSettings::StopMode::kProcess:
      return "Process";
    case BreakpointSettings::StopMode::kAll:
      return "All";
  }
  FXL_NOTREACHED();
  return std::string();
}

const char* BreakpointEnabledToString(bool enabled) {
  return enabled ? "Enabled" : "Disabled";
}

const char* BreakpointTypeToString(debug_ipc::BreakpointType type) {
  switch (type) {
    case debug_ipc::BreakpointType::kSoftware:
      return "Software";
    case debug_ipc::BreakpointType::kHardware:
      return "Hardware";
  }
}

std::string DescribeJobContext(const ConsoleContext* context,
                               const JobContext* job_context) {
  int id = context->IdForJobContext(job_context);
  std::string state = JobContextStateToString(job_context->GetState());

  // Koid string. This includes a trailing space when present so it can be
  // concat'd even when not present and things look nice.
  std::string koid_str;
  if (job_context->GetState() == JobContext::State::kRunning) {
    koid_str = fxl::StringPrintf("koid=%" PRIu64 " ",
                                 job_context->GetJob()->GetKoid());
  }

  std::string result =
      fxl::StringPrintf("Job %d [%s] %s", id, state.c_str(), koid_str.c_str());
  result += DescribeJobContextName(job_context);
  return result;
}

std::string DescribeTarget(const ConsoleContext* context,
                           const Target* target) {
  int id = context->IdForTarget(target);
  std::string state = TargetStateToString(target->GetState());

  // Koid string. This includes a trailing space when present so it can be
  // concat'd even when not present and things look nice.
  std::string koid_str;
  if (target->GetState() == Target::State::kRunning) {
    koid_str =
        fxl::StringPrintf("koid=%" PRIu64 " ", target->GetProcess()->GetKoid());
  }

  std::string result = fxl::StringPrintf("Process %d [%s] %s", id, state.c_str(),
                                         koid_str.c_str());
  result += DescribeTargetName(target);
  return result;
}

std::string DescribeTargetName(const Target* target) {
  // When running, use the object name if any.
  std::string name;
  if (target->GetState() == Target::State::kRunning)
    name = target->GetProcess()->GetName();

  // Otherwise fall back to the program name which is the first arg.
  if (name.empty()) {
    const std::vector<std::string>& args = target->GetArgs();
    if (!args.empty())
      name += args[0];
  }
  return name;
}

std::string DescribeJobContextName(const JobContext* job_context) {
  // When running, use the object name if any.
  std::string name;
  if (job_context->GetState() == JobContext::State::kRunning)
    name = job_context->GetJob()->GetName();

  return name;
}

std::string DescribeThread(const ConsoleContext* context,
                           const Thread* thread) {
  return fxl::StringPrintf(
      "Thread %d [%s] koid=%" PRIu64 " %s", context->IdForThread(thread),
      ThreadStateToString(thread->GetState(), thread->GetBlockedReason())
          .c_str(),
      thread->GetKoid(), thread->GetName().c_str());
}

std::string DescribeBreakpoint(const ConsoleContext* context,
                               const Breakpoint* breakpoint) {
  BreakpointSettings settings = breakpoint->GetSettings();

  std::string scope = BreakpointScopeToString(context, settings);
  std::string stop = BreakpointStopToString(settings.stop_mode);
  const char* enabled = BreakpointEnabledToString(settings.enabled);
  const char* type = BreakpointTypeToString(settings.type);
  std::string location = DescribeInputLocation(settings.location);

  return fxl::StringPrintf("Breakpoint %d (%s) on %s, %s, stop=%s, @ %s",
                           context->IdForBreakpoint(breakpoint), type,
                           scope.c_str(), enabled, stop.c_str(),
                           location.c_str());
}

std::string DescribeInputLocation(const InputLocation& location) {
  switch (location.type) {
    case InputLocation::Type::kNone:
      return "<no location>";
    case InputLocation::Type::kLine:
      return DescribeFileLine(location.line);
    case InputLocation::Type::kSymbol:
    case InputLocation::Type::kElfSymbol:
      return location.symbol;
    case InputLocation::Type::kAddress:
      return fxl::StringPrintf("0x%" PRIx64, location.address);
  }
  FXL_NOTREACHED();
  return std::string();
}

OutputBuffer FormatIdentifier(const std::string& str, bool bold_last) {
  const auto& [err, identifier] = Identifier::FromString(str);
  if (err.has_error()) {
    // Not parseable as an identnfier, just write the string.
    return OutputBuffer(str);
  }

  OutputBuffer result;

  const auto& comps = identifier.components();
  for (size_t i = 0; i < comps.size(); i++) {
    const auto& comp = comps[i];
    if (comp.has_separator())
      result.Append("::");

    // Name.
    if (bold_last && i == comps.size() - 1)
      result.Append(Syntax::kHeading, comp.name().value());
    else
      result.Append(Syntax::kNormal, comp.name().value());

    // Template.
    if (comp.has_template()) {
      std::string t_string;
      t_string += comp.template_begin().value();

      for (size_t t_i = 0; t_i < comp.template_contents().size(); t_i++) {
        if (t_i > 0)
          t_string += ", ";
        t_string += comp.template_contents()[t_i];
      }

      t_string += comp.template_end().value();
      result.Append(Syntax::kComment, std::move(t_string));
    }
  }

  return result;
}

OutputBuffer FormatFunctionName(const Function* function, bool show_params) {
  OutputBuffer result = FormatIdentifier(function->GetFullName(), true);

  const auto& params = function->parameters();
  std::string params_str;
  if (show_params) {
    params_str.push_back('(');
    for (size_t i = 0; i < params.size(); i++) {
      if (i > 0)
        params_str += ", ";
      if (const Variable* var = params[i].Get()->AsVariable())
        params_str += var->type().Get()->GetFullName();
    }
    params_str.push_back(')');
  } else {
    if (params.empty())
      params_str += "()";
    else
      params_str += "(…)";
  }

  result.Append(Syntax::kComment, std::move(params_str));
  return result;
}

OutputBuffer FormatLocation(const Location& loc, bool always_show_address,
                            bool always_show_types) {
  if (!loc.is_valid())
    return OutputBuffer("<invalid address>");
  if (!loc.has_symbols())
    return OutputBuffer(fxl::StringPrintf("0x%" PRIx64, loc.address()));

  OutputBuffer result;
  if (always_show_address) {
    result = OutputBuffer(Syntax::kComment,
                          fxl::StringPrintf("0x%" PRIx64 ", ", loc.address()));
  }

  const Function* func = loc.symbol().Get()->AsFunction();
  if (func) {
    OutputBuffer func_output = FormatFunctionName(func, always_show_types);
    if (!func_output.empty()) {
      result.Append(std::move(func_output));
      if (loc.file_line().is_valid()) {
        // Separator between function and file/line.
        result.Append(" " + GetBullet() + " ");
      } else {
        // Check if the address is inside a function and show the offset.
        AddressRange function_range = func->GetFullRange(loc.symbol_context());
        if (function_range.InRange(loc.address())) {
          // Inside a function but no file/line known. Show the offset.
          result.Append(fxl::StringPrintf(
              " + 0x%" PRIx64, loc.address() - function_range.begin()));
          result.Append(Syntax::kComment, " (no line info)");
        }
      }
    }
  }

  if (loc.file_line().is_valid())
    result.Append(DescribeFileLine(loc.file_line()));
  return result;
}

std::string DescribeFileLine(const FileLine& file_line, bool show_path) {
  std::string result;

  // Name.
  if (file_line.file().empty())
    result = "?";
  else if (show_path)
    result = file_line.file();
  else
    result = file_line.GetFileNamePart();

  result.push_back(':');

  // Line.
  if (file_line.line() == 0)
    result.push_back('?');
  else
    result.append(fxl::StringPrintf("%d", file_line.line()));

  return result;
}

Err SetElementsToAdd(const std::vector<std::string>& args,
                     AssignType* assign_type,
                     std::vector<std::string>* elements_to_set) {
  if (args.size() < 2u)
    return Err("Expected at least two arguments.");

  elements_to_set->clear();

  // Validation.
  auto& token = args[1];
  if (token == "=" || token == "+=" || token == "-=") {
    if (args.size() < 3)
      return Err("Expected a value after \"=\"");
    elements_to_set->insert(elements_to_set->end(), args.begin() + 2,
                            args.end());
    if (token == "=") {
      *assign_type = AssignType::kAssign;
    } else if (token == "+=") {
      *assign_type = AssignType::kAppend;
    }
    if (token == "-=") {
      *assign_type = AssignType::kRemove;
    }
  } else {
    *assign_type = AssignType::kAssign;
    // We just append everything after the setting name.
    elements_to_set->insert(elements_to_set->end(), args.begin() + 1,
                            args.end());
  }

  return Err();
}

const char* AssignTypeToString(AssignType assign_type) {
  switch (assign_type) {
    case AssignType::kAssign:
      return "Assign";
    case AssignType::kAppend:
      return "Append";
    case AssignType::kRemove:
      return "Remove";
  }

  FXL_NOTREACHED();
  return "";
}

}  // namespace zxdb
