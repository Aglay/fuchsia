// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_ZXDB_CLIENT_SYSTEM_OBSERVER_H_
#define GARNET_BIN_ZXDB_CLIENT_SYSTEM_OBSERVER_H_

#include <string>

namespace zxdb {

class Breakpoint;
class Process;
class Target;
class JobContext;

class SystemObserver {
 public:
  // Called immediately after creation / before destruction of a target.
  virtual void DidCreateTarget(Target* target) {}
  virtual void WillDestroyTarget(Target* target) {}

  // Called immediately after creation of a job context.
  virtual void DidCreateJobContext(JobContext* job_context) {}

  // It can be common to want to watch for all Process creation and destruction
  // events. For convenience, these allow that without having to track each
  // Target and register as an observer on them individually.
  virtual void GlobalDidCreateProcess(Process* process) {}
  virtual void GlobalWillDestroyProcess(Process* process) {}

  // Called immediately after creation / before destruction of a breakpoint.
  virtual void DidCreateBreakpoint(Breakpoint* breakpoint) {}
  virtual void WillDestroyBreakpoint(Breakpoint* breakpoint) {}

  // Indicates an informational message from the symbol indexing system.
  // This will be things like "X" symbols loaded from "Y".
  virtual void OnSymbolIndexingInformation(const std::string& msg) {}
};

}  // namespace zxdb

#endif  // GARNET_BIN_ZXDB_CLIENT_SYSTEM_OBSERVER_H_
