// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/input_location_parser.h"

#include <inttypes.h>

#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/string_util.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"

namespace zxdb {

Err ParseInputLocation(const Frame* frame, const std::string& input,
                       InputLocation* location) {
  if (input.empty())
    return Err("Passed empty location.");

  // Check for one colon. Two colons is a C++ member function.
  size_t colon = input.find(':');
  if (colon != std::string::npos && colon < input.size() - 1 &&
      input[colon + 1] != ':') {
    // <file>:<line> format.
    std::string file = input.substr(0, colon);

    uint64_t line = 0;
    Err err = StringToUint64(input.substr(colon + 1), &line);
    if (err.has_error())
      return err;

    location->type = InputLocation::Type::kLine;
    location->line = FileLine(std::move(file), static_cast<int>(line));
    return Err();
  }

  // Check for memory addresses.
  bool is_address = false;
  size_t address_begin = 0;  // Index of address number when is_address.
  if (input[0] == '*') {
    // *<address> format
    is_address = true;
    address_begin = 1;  // Skip "*".
  } else if (CheckHexPrefix(input)) {
    // Hex numbers are addresses.
    is_address = true;
    address_begin = 0;
  }
  if (is_address) {
    std::string addr_str = input.substr(address_begin);
    Err err = StringToUint64(addr_str, &location->address);
    if (err.has_error())
      return err;

    location->type = InputLocation::Type::kAddress;
    return Err();
  }

  uint64_t line = 0;
  Err err = StringToUint64(input, &line);
  if (err.has_error()) {
    // Not a number, assume symbol.
    location->type = InputLocation::Type::kSymbol;
    location->symbol = input;
    return Err();
  }

  // Just a number, use the file name from the specified frame.
  if (!frame) {
    return Err(
        "There is no current frame to get a file name, you'll have to "
        "specify an explicit frame or file name.");
  }
  const Location& frame_location = frame->GetLocation();
  if (frame_location.file_line().file().empty()) {
    return Err(
        "The current frame doesn't have a file name to use, you'll "
        "have to specify a file.");
  }
  location->type = InputLocation::Type::kLine;
  location->line =
      FileLine(frame_location.file_line().file(), static_cast<int>(line));
  return Err();
}

Err ResolveInputLocations(const ProcessSymbols* process_symbols,
                          const InputLocation& input_location, bool symbolize,
                          std::vector<Location>* locations) {
  ResolveOptions options;
  options.symbolize = symbolize;
  *locations = process_symbols->ResolveInputLocation(input_location, options);

  if (locations->empty()) {
    return Err("Nothing matching this %s was found.",
               InputLocation::TypeToString(input_location.type));
  }
  return Err();
}

Err ResolveInputLocations(const ProcessSymbols* process_symbols,
                          const Frame* optional_frame, const std::string& input,
                          bool symbolize, std::vector<Location>* locations) {
  InputLocation input_location;
  Err err = ParseInputLocation(optional_frame, input, &input_location);
  if (err.has_error())
    return err;
  return ResolveInputLocations(process_symbols, input_location, symbolize,
                               locations);
}

Err ResolveUniqueInputLocation(const ProcessSymbols* process_symbols,
                               const InputLocation& input_location,
                               bool symbolize, Location* location) {
  std::vector<Location> locations;
  Err err = ResolveInputLocations(process_symbols, input_location, symbolize,
                                  &locations);
  if (err.has_error())
    return err;

  FXL_DCHECK(!locations.empty());  // Non-empty on success should be guaranteed.

  if (locations.size() == 1u) {
    // Success, got a unique location.
    *location = locations[0];
    return Err();
  }

  // When there is more than one, generate an error that lists the
  // possibilities for disambiguation.
  std::string err_str = "This resolves to more than one location. Could be:\n";
  constexpr size_t kMaxSuggestions = 10u;

  if (!symbolize) {
    // The original call did not request symbolization which will produce very
    // non-helpful suggestions. We're not concerned about performance in this
    // error case so re-query to get the full symbols.
    locations.clear();
    ResolveInputLocations(process_symbols, input_location, true, &locations);
  }

  for (size_t i = 0; i < locations.size() && i < kMaxSuggestions; i++) {
    // Always show the full path since we're doing disambiguation and the
    // problem could have been two files with the same name but different
    // paths.
    err_str += fxl::StringPrintf(" %s ", GetBullet().c_str());
    if (locations[i].file_line().is_valid()) {
      err_str += DescribeFileLine(locations[i].file_line(), true);
      err_str += fxl::StringPrintf(" = 0x%" PRIx64, locations[i].address());
    } else {
      err_str += FormatLocation(locations[i], true, false).AsString();
    }
    err_str += "\n";
  }
  if (locations.size() > kMaxSuggestions) {
    err_str += fxl::StringPrintf("...%zu more omitted...\n",
                                 locations.size() - kMaxSuggestions);
  }
  return Err(err_str);
}

Err ResolveUniqueInputLocation(const ProcessSymbols* process_symbols,
                               const Frame* optional_frame,
                               const std::string& input, bool symbolize,
                               Location* location) {
  InputLocation input_location;
  Err err = ParseInputLocation(optional_frame, input, &input_location);
  if (err.has_error())
    return err;
  return ResolveUniqueInputLocation(process_symbols, input_location, symbolize,
                                    location);
}

}  // namespace zxdb
