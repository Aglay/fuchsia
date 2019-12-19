// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_CMD_COMMAND_H_
#define SRC_DEVELOPER_CMD_COMMAND_H_

#include <string>
#include <vector>

namespace cmd {

class Command {
 public:
  Command();
  ~Command();

  Command(const Command&) = delete;
  Command& operator=(const Command&) = delete;

  Command(Command&&) = default;
  Command& operator=(Command&&) = default;

  void Parse(const std::string& line);
  const std::vector<std::string>& args() const { return args_; }

  bool is_empty() const { return args_.empty(); }

 private:
  std::vector<std::string> args_;
};

}  // namespace cmd

#endif  // SRC_DEVELOPER_CMD_COMMAND_H_
