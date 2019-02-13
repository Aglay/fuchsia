// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>

#include <lib/async/cpp/exception.h>
#include <lib/async/cpp/wait.h>

#include "lib/fxl/macros.h"

namespace debug_ipc {

// This signal on the task_event_ indicates there is work to do.
constexpr uint32_t kTaskSignal = ZX_USER_SIGNAL_0;

// 0 is an invalid ID for watchers, so is safe to use here.
constexpr uint64_t kTaskSignalKey = 0;

// Group of classes dedicated at handling async events associated with zircon's
// message loop.

enum class WatchType {
  kTask,
  kFdio,
  kProcessExceptions,
  kJobExceptions,
  kSocket
};
const char* WatchTypeToString(WatchType);

class SignalHandler {
 public:
  static void Handler(async_dispatcher_t*, async_wait_t*, zx_status_t,
                      const zx_packet_signal_t*);

  SignalHandler();
  explicit SignalHandler(int id, zx_handle_t object, zx_signals_t signals);
  ~SignalHandler();

  FXL_DISALLOW_COPY_AND_ASSIGN(SignalHandler);

  SignalHandler(SignalHandler&&);
  SignalHandler& operator=(SignalHandler&&);

  int watch_info_id() const { return watch_info_id_; }
  const async_wait_t* handle() const { return handle_.get(); }

 private:
  void WaitForSignals() const;

  int watch_info_id_ = -1;
  std::unique_ptr<async_wait_t> handle_;
};

class ExceptionHandler {
 public:
  static void Handler(async_dispatcher_t*, async_exception_t*,
                      zx_status_t status, const zx_port_packet_t* packet);

  ExceptionHandler();
  explicit ExceptionHandler(int id, zx_handle_t object, uint32_t options);
  ~ExceptionHandler();

  FXL_DISALLOW_COPY_AND_ASSIGN(ExceptionHandler);

  ExceptionHandler(ExceptionHandler&&);
  ExceptionHandler& operator=(ExceptionHandler&&);

  int watch_info_id() const { return watch_info_id_; }
  const async_exception_t* handle() const { return handle_.get(); }

 private:
  int watch_info_id_ = -1;
  std::unique_ptr<async_exception_t> handle_;
};

}  // namespace debug_ipc
