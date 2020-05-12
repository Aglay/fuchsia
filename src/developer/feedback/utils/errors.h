// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_UTILS_ERRORS_H_
#define SRC_DEVELOPER_FEEDBACK_UTILS_ERRORS_H_

#include <lib/syslog/cpp/macros.h>

#include <string>

namespace feedback {

// Defines common errors that occur throughout //src/developer/feedback.
enum class Error {
  kNotSet,
  // TODO(49922): Remove kDefault. This value is temporary to allow the enum to be used without
  // specifying the exact error that occurred.
  kDefault,
  kLogicError,
  kTimeout,
  kConnectionError,
  kAsyncTaskPostFailure,
  kMissingValue,
  kBadValue,
  kFileReadFailure,
  kFileWriteFailure,
};

// Provide a string representation of  |error|.
inline std::string ToString(Error error) {
  switch (error) {
    case Error::kNotSet:
      return "Error::kNotSet";
    case Error::kDefault:
      return "Error::kDefault";
    case Error::kLogicError:
      return "Error::kLogicError";
    case Error::kTimeout:
      return "Error::kTimeout";
    case Error::kConnectionError:
      return "Error::kConnectionError";
    case Error::kAsyncTaskPostFailure:
      return "Error::kAsyncTaskPostFailure";
    case Error::kMissingValue:
      return "Error::kMissingValue";
    case Error::kBadValue:
      return "Error::kBadValue";
    case Error::kFileReadFailure:
      return "Error::kFileReadFailure";
    case Error::kFileWriteFailure:
      return "Error::kFileWriteFailure";
  }
}

// Provide a reason why |error| occurred.
inline std::string ToReason(const Error error) {
  switch (error) {
    case Error::kLogicError:
      return "feedback logic error";
    case Error::kTimeout:
      return "data collection timeout";
    case Error::kConnectionError:
      return "FIDL connection error";
    case Error::kAsyncTaskPostFailure:
      return "async post task failure";
    case Error::kMissingValue:
      return "no data returned";
    case Error::kBadValue:
      return "bad data returned";
    case Error::kFileReadFailure:
      return "file read failure";
    case Error::kFileWriteFailure:
      return "file write failure";
    case Error::kDefault:
      FX_LOGS(FATAL) << "Error::kDefault does not have a reason";
    case Error::kNotSet:
      FX_LOGS(FATAL) << "Error::kNotSet does not have a reason";
      return "FATAL, THIS SHOULD NOT HAPPEN";
  }
}

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_UTILS_ERRORS_H_
