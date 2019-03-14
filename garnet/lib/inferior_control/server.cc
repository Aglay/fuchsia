// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "server.h"

#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>

#include "lib/fxl/logging.h"
#include "lib/fxl/strings/string_printf.h"

#include "garnet/lib/debugger_utils/breakpoints.h"
#include "garnet/lib/debugger_utils/threads.h"
#include "garnet/lib/debugger_utils/util.h"

namespace inferior_control {

Server::Server(zx::job job_for_search, zx::job job_for_launch)
    : job_for_search_(std::move(job_for_search)),
      job_for_launch_(std::move(job_for_launch)),
      message_loop_(&kAsyncLoopConfigNoAttachToThread),
      exception_port_(message_loop_.dispatcher(),
                      fit::bind_member(this, &Server::OnProcessException),
                      fit::bind_member(this, &Server::OnProcessSignal)),
      run_status_(true) {}

Server::~Server() {}

void Server::SetCurrentThread(Thread* thread) {
  if (!thread)
    current_thread_.reset();
  else
    current_thread_ = thread->AsWeakPtr();
}

void Server::QuitMessageLoop(bool status) {
  run_status_ = status;
  message_loop_.Quit();
}

void Server::PostQuitMessageLoop(bool status) {
  run_status_ = status;
  async::PostTask(message_loop_.dispatcher(), [this] { message_loop_.Quit(); });
}

void Server::WaitAsync(Thread* thread) {
  exception_port_.WaitAsync(thread);
}

void Server::OnProcessException(const zx_port_packet_t& packet) {
  // At the moment we only support one process.
  Process* process = current_process();
  FXL_DCHECK(process);
  FXL_DCHECK(ZX_PKT_IS_EXCEPTION(packet.type));

  zx_excp_type_t type = static_cast<zx_excp_type_t>(packet.type);
  zx_koid_t tid = packet.exception.tid;
  Thread* thread = nullptr;
  if (tid != ZX_KOID_INVALID) {
    thread = process->FindThreadById(tid);
  }

  // If |thread| is nullptr then the thread must have just terminated,
  // and there's nothing to do. The process itself could also have terminated.
  if (thread == nullptr) {
    // Alas there's no robust test to verify it just terminated,
    // we just have to assume it.
    FXL_LOG(WARNING) << "Thread " << tid << " not found, terminated";
    return;
  }

  // At this point the thread is either an existing thread or a new thread
  // which has been fully registered in our database.

  // Manage loading of dso info.
  // At present this is only done at startup. TODO(dje): dlopen.
  // This is done by setting ZX_PROCESS_DEBUG_ADDR_BREAK_ON_SET which causes
  // a s/w breakpoint instruction to be executed after all dsos are loaded.
  // TODO(dje): Handle case of hitting a breakpoint before then (highly
  // unlikely, but technically possible).
  if (type == ZX_EXCP_SW_BREAKPOINT) {
    if (process->CheckDsosList(thread)) {
      zx_status_t status =
        debugger_utils::ResumeAfterSoftwareBreakpointInstruction(
            thread->handle(), exception_port_.handle());
      if (status != ZX_OK) {
        FXL_LOG(ERROR) << "Unable to resume thread " << thread->GetName()
                       << ", status: "
                       << debugger_utils::ZxErrorString(status);
      }
      // This is a breakpoint we introduced. No point in passing it on to
      // other handlers. If resumption fails there's not much we can do.
      return;
    }
  }

  zx_exception_report_t report;
  zx_status_t status = thread->GetExceptionReport(&report);
  if (status == ZX_ERR_BAD_STATE) {
    // Nothing more to do, let process cleanup finish things up.
    return;
  }
  const zx_exception_context_t& context = report.context;

  Delegate* delegate = process->delegate();

  // First update our internal state for the thread.
  thread->OnException(type, context);

  // |type| could either map to an architectural exception or Zircon-defined
  // synthetic exceptions.
  if (ZX_EXCP_IS_ARCH(type)) {
    delegate->OnArchitecturalException(process, thread, type, context);
    return;
  }

  // Must be a synthetic exception.
  switch (type) {
    case ZX_EXCP_THREAD_STARTING:
      delegate->OnThreadStarting(process, thread, context);
      break;
    case ZX_EXCP_THREAD_EXITING:
      delegate->OnThreadExiting(process, thread, context);
      break;
    case ZX_EXCP_POLICY_ERROR:
      delegate->OnSyntheticException(process, thread, type, context);
      break;
    default:
      FXL_LOG(ERROR) << "Ignoring unrecognized synthetic exception for thread "
                     << tid << ": " << type;
      break;
  }
}

void Server::OnProcessSignal(const zx_port_packet_t& packet) {
  // At the moment we only support one process.
  Process* process = current_process();
  FXL_DCHECK(process);
  FXL_DCHECK(packet.type == ZX_PKT_TYPE_SIGNAL_ONE);

  uint64_t key = packet.key;
  FXL_VLOG(2) << "Received ZX_PKT_TYPE_SIGNAL_ONE, observed 0x" << std::hex
              << packet.signal.observed << ", key " << std::dec << key;
  // Process exit is sent as a regular signal.
  if (key == process->id()) {
    if (packet.signal.observed & ZX_PROCESS_TERMINATED) {
      process->OnTermination();
      // No point in installing another async-wait, process is dead.
    }
  }

  Thread* thread = process->FindThreadById(key);
  if (thread == nullptr) {
    FXL_LOG(WARNING) << "Unexpected signal key: " << key;
    return;
  }
  thread->OnSignal(packet.signal.observed);
  // Async-waits must be continually re-registered.
  if (!(packet.signal.observed & ZX_THREAD_TERMINATED)) {
    WaitAsync(thread);
  }
}

ServerWithIO::ServerWithIO(zx::job job_for_search, zx::job job_for_launch)
    : Server(std::move(job_for_search), std::move(job_for_launch)),
      client_sock_(-1) {}

ServerWithIO::~ServerWithIO() {
  // This will invoke the IOLoop destructor which will clean up and join the
  // I/O threads. This is done now because |message_loop_| and |client_sock_|
  // must outlive |io_loop_|. The former is handled by virtue of being in the
  // baseclass. The latter is handled here.
  io_loop_.reset();
}

}  // namespace inferior_control
