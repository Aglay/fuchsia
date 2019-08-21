// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_SHARED_LOGGING_LOGGING_H_
#define SRC_DEVELOPER_DEBUG_SHARED_LOGGING_LOGGING_H_

// This header is meant to be the hub of debug logging: timers, logging, etc. There is no need to
// include the other headers directly.

#include <sstream>

#include "src/developer/debug/shared/logging/block_timer.h"
#include "src/developer/debug/shared/logging/macros.h"
#include "src/developer/debug/shared/logging/debug.h"
#include "src/developer/debug/shared/logging/file_line_function.h"

namespace debug_ipc {

// Normally you would use this macro to create logging statements.
// Example:
//
// DEBUG_LOG(Job) << "Some job statement.";
//
// DEBUG_LOG(MessageLoop) << "Some event with id " << id;
#define DEBUG_LOG(category)                                   \
  ::debug_ipc::LogStatement STRINGIFY(__debug_log, __LINE__)( \
      FROM_HERE, ::debug_ipc::LogCategory::k##category);      \
  STRINGIFY(__debug_log, __LINE__).stream()

// Creates a conditional logger depending whether the debug mode is active or not. See debug.h for
// more details.
class LogStatement {
 public:
  explicit LogStatement(FileLineFunction origin, LogCategory);
  ~LogStatement();

  std::ostream& stream() { return stream_; }
  std::string GetMsg();

  const FileLineFunction& origin() const { return origin_; }
  LogCategory category() const { return category_; }
  double time() const { return time_; }


 private:
  FileLineFunction origin_;
  LogCategory category_;
  bool should_log_ = false;
  double time_;     // When the event occurred.

  std::ostringstream stream_;
};

}  // namespace debug_ipc

#endif  // SRC_DEVELOPER_DEBUG_SHARED_LOGGING_LOGGING_H_
