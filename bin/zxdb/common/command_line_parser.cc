// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/common/command_line_parser.h"

#include <algorithm>

namespace zxdb {

namespace {

// Returns true if the argument is the special string that indicates the end
// of options.
bool IsOptionEndFlag(const char* arg) { return strcmp(arg, "--") == 0; }

// Checks if the given argument is a short option and if it is, returns the
// letter. If not, returns 0.
char GetShortOption(const char* arg, const char** value_begin) {
  if (arg[0] == '-' && arg[1] != 0 && arg[1] != '-') {
    *value_begin = &arg[2];  // Arg (if any) follows immediately.
    return arg[1];
  }
  *value_begin = nullptr;
  return 0;
}

// Checks if the given argument is a long option and returns it, not including
// the preceding "--". If it's not an option, returns the empty string. To
// differentiate args comsisting of only "--" and non-options, callers should
// call IsOptionEndFlag() before this.
std::string GetLongOption(const char* arg, const char** value_begin) {
  *value_begin = nullptr;
  if (arg[0] != '-' || arg[1] != '-')
    return std::string();

  // See if there's and "=<arg>" in this flag.
  const char* equals = strchr(arg, '=');
  if (!equals)
    return &arg[2];  // No "=<arg>".

  // value_begin gets everything after the equals.
  *value_begin = &equals[1];

  // Return everything between the '--' (2 bytes) and the equals.
  return std::string(&arg[2], equals - arg - 2);
}

}  // namespace

GeneralCommandLineParser::GeneralCommandLineParser() = default;
GeneralCommandLineParser::~GeneralCommandLineParser() = default;

void GeneralCommandLineParser::AddGeneralSwitch(const char* long_name,
                                                const char short_name,
                                                const char* help,
                                                NoArgCallback cb) {
  Record& record = records_.emplace_back();
  record.long_name = long_name;
  record.short_name = short_name;
  record.help_text = help;
  record.no_arg_callback = std::move(cb);
}

void GeneralCommandLineParser::AddGeneralSwitch(const char* long_name,
                                                const char short_name,
                                                const char* help,
                                                StringCallback cb) {
  Record& record = records_.emplace_back();
  record.long_name = long_name;
  record.short_name = short_name;
  record.help_text = help;
  record.string_callback = std::move(cb);
}

std::string GeneralCommandLineParser::GetHelp() const {
  std::vector<std::string> switches;
  for (const auto& record : records_)
    switches.push_back(record.help_text);

  std::sort(switches.begin(), switches.end());

  std::string result;
  for (const std::string& str : switches) {
    result.append(str);
    result.append("\n\n");
  }
  return result;
}

Err GeneralCommandLineParser::ParseGeneral(
    int argc, const char* argv[], std::vector<std::string>* params) const {
  // Expect argv[0] to be the program itself.
  if (argc <= 1)
    return Err();

  int last_option_index = argc - 1;
  for (int i = 1; i < argc; i++) {
    // Non-null when we find the argument.
    const char* arg_begin = nullptr;

    const Record* record = nullptr;
    if (IsOptionEndFlag(argv[i])) {
      // End of option indicator.
      last_option_index = i;
      break;
    } else if (char c = GetShortOption(argv[i], &arg_begin)) {
      // Single-letter option.
      for (const Record& cur_record : records_) {
        if (cur_record.short_name == c) {
          record = &cur_record;
          break;
        }
      }
    } else if (auto long_opt = GetLongOption(argv[i], &arg_begin);
               !long_opt.empty()) {
      // Long option.
      for (const Record& cur_record : records_) {
        if (cur_record.long_name == long_opt) {
          record = &cur_record;
          break;
        }
      }
    } else {
      // Non-option.
      last_option_index = i - 1;
      break;
    }

    // If we get here we should have found a record for the option.
    if (!record)
      return Err("%s is not a valid option. Try --help", argv[i]);

    if (NeedsArg(record)) {
      // Arguments can be already found ("-cfoo" or "--foo=bar") or they could
      // be the following parameter.
      if (!arg_begin || !*arg_begin) {
        // Argument is in the next token of the command line.
        if (i == argc - 1)
          return Err("%s expects an argument but none was given.\n\n%s",
                     argv[i], record->help_text);
        i++;
        arg_begin = argv[i];
      }
    } else {
      // Don't expect an arg for this switch.
      if (arg_begin && *arg_begin) {
        // Arg points somewhere other than the end of the string when we
        // weren't expecting an arg.
        return Err(
            "Unexpected value for argument that doesn't take one:\n"
            "  %s\n\n%s",
            argv[i], record->help_text);
      }
    }

    // Execute the right callback.
    Err err;
    if (record->string_callback)
      err = record->string_callback(arg_begin);
    else if (record->no_arg_callback)
      record->no_arg_callback();

    if (err.has_error())
      return err;
  }

  // Everything else following the parameters are the positional arguments.
  for (int i = last_option_index + 1; i < argc; i++)
    params->push_back(argv[i]);
  return Err();
}

// static
bool GeneralCommandLineParser::NeedsArg(const Record* record) {
  return !!record->string_callback;
}

}  // namespace zxdb
