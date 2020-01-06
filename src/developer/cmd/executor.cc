// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/cmd/executor.h"

#include <lib/fdio/spawn.h>
#include <lib/zx/process.h>

#include <map>
#include <memory>
#include <vector>

#include "src/developer/cmd/tasks/cd_task.h"
#include "src/developer/cmd/tasks/getenv_task.h"
#include "src/developer/cmd/tasks/process_task.h"
#include "src/developer/cmd/tasks/quit_task.h"
#include "src/developer/cmd/tasks/setenv_task.h"
#include "src/developer/cmd/tasks/unsetenv_task.h"

namespace cmd {
namespace {

template <typename TaskType>
std::unique_ptr<Task> CreateTask(async_dispatcher_t* dispatcher) {
  return std::make_unique<TaskType>(dispatcher);
}

}  // namespace

Executor::Executor(async_dispatcher_t* dispatcher)
    : dispatcher_(dispatcher),
      builtin_commands_({
          {"cd", &CreateTask<CdTask>},
          {"exit", &CreateTask<QuitTask>},
          {"getenv", &CreateTask<GetenvTask>},
          {"quit", &CreateTask<QuitTask>},
          {"setenv", &CreateTask<SetenvTask>},
          {"unsetenv", &CreateTask<UnsetenvTask>},
      }) {}

Executor::~Executor() = default;

zx_status_t Executor::Execute(Command command, Task::CompletionCallback callback) {
  if (command.is_empty()) {
    return ZX_ERR_NEXT;
  }

  current_task_ = FindAndCreateTask(command.args()[0]);
  return current_task_->Execute(std::move(command), std::move(callback));
}

void Executor::Complete(Autocomplete* autocomplete) {
  if (autocomplete->tokens().empty()) {
    CompleteCommand(autocomplete);
    return;
  }
  std::vector<std::string> result;
  FindAndCreateTask(autocomplete->tokens().front())->Complete(autocomplete);
}

std::unique_ptr<Task> Executor::FindAndCreateTask(const std::string& name) {
  auto it = builtin_commands_.find(name);
  if (it != builtin_commands_.end()) {
    return it->second(dispatcher_);
  } else {
    return std::make_unique<ProcessTask>(dispatcher_);
  }
}

void Executor::CompleteCommand(Autocomplete* autocomplete) {
  std::vector<std::string> result;
  for (const auto& entry : builtin_commands_) {
    const std::string& key = entry.first;
    if (key.find(autocomplete->fragment()) == 0u) {
      autocomplete->AddCompletion(key);
    }
  }
  ProcessTask::CompleteCommand(autocomplete);
}

}  // namespace cmd
