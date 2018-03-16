// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <string>

namespace zxdb {

class Command;
class ConsoleContext;
class Err;
class Target;

[[nodiscard]] Err StringToUint64(const std::string& s, uint64_t* out);

// Reads an int64 from the given index of the command args. Returns an error
// if there are not enough args, or if the value isn't an int64.
//
// The param_desc will be used in the error string, for example "process koid".
[[nodiscard]] Err ReadUint64Arg(const Command& cmd, size_t arg_index,
                                const char* param_desc, uint64_t* out);

// Returns a string describing the given target in the given context. If
// columns is set, there will be extra padding added so that multiple targets
// line up when printed vertically.
std::string DescribeTarget(ConsoleContext* context, Target* target,
                           bool columns);

}  // namespace zxdb
