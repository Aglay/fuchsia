// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "garnet/bin/zxdb/client/client_object.h"
#include "garnet/bin/zxdb/client/frame_fingerprint.h"
#include "garnet/bin/zxdb/client/setting_store.h"
#include "garnet/bin/zxdb/client/stack.h"
#include "garnet/bin/zxdb/client/thread_observer.h"
#include "garnet/lib/debug_ipc/protocol.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "lib/fxl/observer_list.h"

namespace zxdb {

class Err;
class Frame;
class Process;
class RegisterSet;
class ThreadController;

// The flow control commands on this object (Pause, Continue, Step...) apply
// only to this thread (other threads will continue to run or not run
// as they were previously).
class Thread : public ClientObject {
 public:
  explicit Thread(Session* session);
  ~Thread() override;

  void AddObserver(ThreadObserver* observer);
  void RemoveObserver(ThreadObserver* observer);

  fxl::WeakPtr<Thread> GetWeakPtr();

  // Guaranteed non-null.
  virtual Process* GetProcess() const = 0;

  virtual uint64_t GetKoid() const = 0;
  virtual const std::string& GetName() const = 0;

  // The state of the thread isn't necessarily up-to-date. There are no
  // system messages for a thread transitioning to suspended, for example.
  // To make sure this is up-to-date, call Process::SyncThreads() or
  // Thread::SyncFrames().
  virtual debug_ipc::ThreadRecord::State GetState() const = 0;
  virtual debug_ipc::ThreadRecord::BlockedReason GetBlockedReason() const = 0;

  virtual void Pause() = 0;
  virtual void Continue() = 0;

  // Continues the thread using the given ThreadController. This is used
  // to implement the more complex forms of stepping.
  //
  // The on_continue callback does NOT indicate that the thread stopped again.
  // This is because many thread controllers may need to do asynchronous setup
  // that could fail. It is issued when the thread is actually resumed or when
  // the resumption fails.
  //
  // The on_continue callback may be issued reentrantly from within the stack
  // of the ContinueWith call if the controller was ready synchronously.
  //
  // On failure the ThreadController will be removed and the thread will not
  // be continued.
  virtual void ContinueWith(std::unique_ptr<ThreadController> controller,
                            std::function<void(const Err&)> on_continue) = 0;

  // Notification from a ThreadController that it has completed its job. The
  // thread controller should be removed from this thread and deleted.
  virtual void NotifyControllerDone(ThreadController* controller) = 0;

  virtual void StepInstruction() = 0;

  // Returns the stack object associated with this thread.
  virtual const Stack& GetStack() const = 0;
  virtual Stack& GetStack() = 0;

  // Obtains the state of the registers for a particular thread.
  // The thread must be stopped in order to get the values.
  //
  // The returned structures are architecture independent, but the contents
  // will be dependent on the architecture the target is running on.
  virtual void ReadRegisters(
      std::vector<debug_ipc::RegisterCategory::Type> cats_to_get,
      std::function<void(const Err&, const RegisterSet&)>) = 0;

  // Provides the setting schema for this object.
  static fxl::RefPtr<SettingSchema> GetSchema();

  SettingStore& settings() { return settings_; }

 protected:
  fxl::ObserverList<ThreadObserver>& observers() { return observers_; }

  SettingStore settings_;

 private:
  fxl::ObserverList<ThreadObserver> observers_;
  fxl::WeakPtrFactory<Thread> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Thread);
};

}  // namespace zxdb
