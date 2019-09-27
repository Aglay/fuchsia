// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "src/developer/debug/debug_agent/debugged_thread.h"

namespace debug_agent {

class MockThread : public DebuggedThread {
 public:
  MockThread(DebuggedProcess* process, zx_koid_t thread_koid,
             std::shared_ptr<ObjectProvider> object_provider);

  void ResumeException() override;
  void ResumeSuspension() override;

  bool Suspend(bool synchronous = false) override;
  bool WaitForSuspension(zx::time deadline = DefaultSuspendDeadline()) override;

  void FillThreadRecord(debug_ipc::ThreadRecord::StackAmount stack_amount,
                        const zx_thread_state_general_regs* optional_regs,
                        debug_ipc::ThreadRecord* record) const override;

  bool IsSuspended() const override { return internal_suspension_ || suspend_count_ > 0; }
  bool IsInException() const override { return in_exception_; }

  bool internal_suspension() const { return internal_suspension_; }
  int suspend_count() const { return suspend_count_; }

 protected:
  void IncreaseSuspend() override;
  void DecreaseSuspend() override;

 private:
  bool internal_suspension_ = false;
  int suspend_count_ = 0;
  bool in_exception_ = false;
};

}  // namespace debug_agent
