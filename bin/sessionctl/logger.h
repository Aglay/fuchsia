#ifndef PERIDOT_BIN_SESSIONCTL_LOGGER_H_
#define PERIDOT_BIN_SESSIONCTL_LOGGER_H_

#include <iostream>
#include <string>

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/future.h>
#include <lib/async/cpp/task.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/strings/string_printf.h>
#include "lib/fxl/functional/make_copyable.h"

namespace modular {

class Logger {
 public:
  explicit Logger(bool json_out);

  void LogError(const std::string& command, const std::string& error) const;

  void Log(const std::string& command,
           const std::map<std::string, std::string>& params) const;

 private:
  bool json_out_;
};

}  // namespace modular

#endif  // PERIDOT_BIN_SESSIONCTL_LOGGER_H_
