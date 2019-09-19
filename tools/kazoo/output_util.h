// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_KAZOO_OUTPUT_UTIL_H_
#define TOOLS_KAZOO_OUTPUT_UTIL_H_

#include "src/lib/fxl/strings/ascii.h"
#include "tools/kazoo/syscall_library.h"
#include "tools/kazoo/writer.h"

// Outputs a copyright header like the one at the top of this file to |writer|.
// true on success, or false with an error logged.
bool CopyrightHeaderWithCppComments(Writer* writer);

// Outputs a copyright header using '#' as the comment marker. Returns true on
// success, or false with an error logged.
bool CopyrightHeaderWithHashComments(Writer* writer);

// Converts |input| to lowercase, assuming it's entirely ASCII.
std::string ToLowerAscii(const std::string& input);

// Maps a name from typical FidlCamelStyle to zircon_snake_style.
std::string CamelToSnake(const std::string& camel_fidl);

// Gets a string representing |type| suitable for output to a C file in userspace.
std::string GetCUserModeName(const Type& type);

// Gets a string representing |type| suitable for output to a C file in a kernel header (rather than
// zx_xyz_t*, this will have user_out_ptr<xyz>, etc.)
std::string GetCKernelModeName(const Type& type);

// Emits a C header declaration for a syscall.
// |prefix| is a string that goes before the entire declaration.
// |name_prefix| is prepended to the function name.
void CDeclaration(const Syscall& syscall, const char* prefix, const char* name_prefix,
                  Writer* writer);

#endif  // TOOLS_KAZOO_OUTPUT_UTIL_H_
