// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_SYSTEM_LOG_RECORDER_WRITER_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_SYSTEM_LOG_RECORDER_WRITER_H_

#include <string>
#include <vector>

#include "src/developer/feedback/feedback_data/system_log_recorder/log_message_store.h"
#include "src/developer/feedback/utils/file_size.h"
#include "src/developer/feedback/utils/write_only_file.h"

namespace feedback {

// Consumes the full content of a store on request, writing it to a rotating set of files.
class SystemLogWriter {
 public:
  SystemLogWriter(const std::vector<const std::string>& log_file_paths, FileSize total_log_size,
                  LogMessageStore* store);

  void Write();

 private:
  // Deletes the last log file and shifts the remaining log files by one position: The first file
  // becomes the second file, the second file becomes the third file, and so on.
  void RotateFilePaths();

  // Truncates the first file to start anew.
  void StartNewFile();

  const std::vector<const std::string> file_paths_;
  const FileSize individual_file_size_;

  WriteOnlyFile current_file_;
  LogMessageStore* store_;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_SYSTEM_LOG_RECORDER_WRITER_H_
