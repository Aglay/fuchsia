// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_UTILS_ERRORS_H_
#define SRC_DEVELOPER_FEEDBACK_UTILS_ERRORS_H_

namespace feedback {

#include <string>

// Defines common errors that occur throughout //src/developer/feedback.
enum class Error {
  kNotSet,
  // TODO(49922): Remove kDefault. This value is temporary to allow the enum to be used without
  // specifying the exact error that occurred.
  kDefault,
  kTimeout,
  kConnectionError,
  kAsyncTaskPostFailure,
  kMissingValue,
  kBadValue,
  kFileReadFailure,
  kFileWriteFailure,
};

inline std::string ToString(Error error) {
  switch (error) {
    case Error::kNotSet:
      return "Error::kNotSet";
    case Error::kDefault:
      return "Error::kDefault";
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

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_UTILS_ERRORS_H_
