// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "garnet/bin/zxdb/client/target.h"

namespace zxdb {

class TargetObserver {
 public:
  // Reason for destroying a process object.
  enum class DestroyReason { kExit, kDetach, kKill };

  // The process could have been newly launched or attached to an existing
  // process.
  //
  // The autoattached_to_new_process flag will be set when this process is
  // a result of attaching automatically to a new process in a job. The
  // process in this state will have not technically started running yet.
  virtual void DidCreateProcess(Target* target, Process* process,
                                bool autoattached_to_new_process) {}

  // Called after detaching from or destroying a process. The Process object
  // will exist but the Target object will report there is no process
  // currently running. The exit code will only have meaning when reason ==
  // kExit, otherwise it will be 0.
  virtual void WillDestroyProcess(Target* target, Process* process,
                                  DestroyReason reason, int exit_code) {}
};

}  // namespace zxdb
