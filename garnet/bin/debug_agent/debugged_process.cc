// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/debug_agent/debugged_process.h"

#include <inttypes.h>
#include <zircon/syscalls/exception.h>
#include <utility>

#include "garnet/bin/debug_agent/debug_agent.h"
#include "garnet/bin/debug_agent/debugged_thread.h"
#include "garnet/bin/debug_agent/object_util.h"
#include "garnet/bin/debug_agent/process_breakpoint.h"
#include "garnet/bin/debug_agent/process_info.h"
#include "garnet/bin/debug_agent/process_memory_accessor.h"
#include "garnet/bin/debug_agent/process_watchpoint.h"
#include "garnet/lib/debug_ipc/agent_protocol.h"
#include "garnet/lib/debug_ipc/debug/logging.h"
#include "garnet/lib/debug_ipc/helper/message_loop_target.h"
#include "garnet/lib/debug_ipc/helper/zx_status.h"
#include "garnet/lib/debug_ipc/message_reader.h"
#include "garnet/lib/debug_ipc/message_writer.h"
#include "lib/fxl/logging.h"

namespace debug_agent {

DebuggedProcess::DebuggedProcess(DebugAgent* debug_agent, zx_koid_t koid,
                                 zx::process proc, bool resume_initial_thread)
    : debug_agent_(debug_agent),
      koid_(koid),
      process_(std::move(proc)),
      resume_initial_thread_(resume_initial_thread),
      waiting_for_initial_thread_(true) {
  // set this property so we can know about module loads.
  const intptr_t kMagicValue = ZX_PROCESS_DEBUG_ADDR_BREAK_ON_SET;
  zx_object_set_property(process_.get(), ZX_PROP_PROCESS_DEBUG_ADDR,
                         &kMagicValue, sizeof(kMagicValue));
}

DebuggedProcess::~DebuggedProcess() { DetachFromProcess(); }

void DebuggedProcess::DetachFromProcess() {
  // 1. Remove installed breakpoints.
  //    We need to tell each thread that this will happen.
  for (auto& [address, breakpoint] : breakpoints_) {
    for (auto& [thread_koid, thread] : threads_) {
      thread->WillDeleteProcessBreakpoint(breakpoint.get());
    }
  }

  breakpoints_.clear();

  // 2. Resume threads.
  // Technically a 0'ed request would work, but being explicit is future-proof.
  debug_ipc::ResumeRequest resume_request = {};
  resume_request.how = debug_ipc::ResumeRequest::How::kContinue;
  resume_request.process_koid = koid_;
  OnResume(resume_request);

  // 3. Unbind from the exception port.
  process_watch_handle_.StopWatching();
}

zx_status_t DebuggedProcess::Init() {
  debug_ipc::MessageLoopTarget* loop = debug_ipc::MessageLoopTarget::Current();
  FXL_DCHECK(loop);  // Loop must be created on this thread first.

  // Register for debug exceptions.
  debug_ipc::MessageLoopTarget::WatchProcessConfig config;
  config.process_name = NameForObject(process_);
  config.process_handle = process_.get();
  config.process_koid = koid_;
  config.watcher = this;
  return loop->WatchProcessExceptions(std::move(config),
                                      &process_watch_handle_);
}

void DebuggedProcess::OnPause(const debug_ipc::PauseRequest& request) {
  if (request.thread_koid) {
    DebuggedThread* thread = GetThread(request.thread_koid);
    if (thread)
      thread->Pause();
    // Could be not found if there is a race between the thread exiting and
    // the client sending the request.
  } else {
    // 0 thread ID means resume all in process.
    PauseAll();
  }
}

void DebuggedProcess::OnResume(const debug_ipc::ResumeRequest& request) {
  if (request.thread_koids.empty()) {
    // Empty thread ID list means resume all threads.
    for (const auto& pair : threads_)
      pair.second->Resume(request);
  } else {
    for (uint64_t thread_koid : request.thread_koids) {
      DebuggedThread* thread = GetThread(thread_koid);
      if (thread)
        thread->Resume(request);
      // Could be not found if there is a race between the thread exiting and
      // the client sending the request.
    }
  }
}

void DebuggedProcess::OnReadMemory(const debug_ipc::ReadMemoryRequest& request,
                                   debug_ipc::ReadMemoryReply* reply) {
  ReadProcessMemoryBlocks(process_, request.address, request.size,
                          &reply->blocks);

  // Remove any breakpoint instructions we've inserted.
  //
  // If there are a lot of ProcessBreakpoints this will get slow. If we find
  // we have 100's of breakpoints an auxiliary data structure could be added
  // to find overlapping breakpoints faster.
  for (const auto& [addr, bp] : breakpoints_) {
    // Generally there will be only one block. If we start reading many
    // megabytes that cross mapped memory boundaries, a top-level range check
    // would be a good idea to avoid unnecessary iteration.
    for (auto& block : reply->blocks)
      bp->FixupMemoryBlock(&block);
  }
}

void DebuggedProcess::OnKill(const debug_ipc::KillRequest& request,
                             debug_ipc::KillReply* reply) {
  reply->status = process_.kill();
}

DebuggedThread* DebuggedProcess::GetThread(zx_koid_t thread_koid) const {
  auto found_thread = threads_.find(thread_koid);
  if (found_thread == threads_.end())
    return nullptr;
  return found_thread->second.get();
}

std::vector<DebuggedThread*> DebuggedProcess::GetThreads() const {
  std::vector<DebuggedThread*> threads;
  threads.reserve(threads_.size());
  for (auto& kv : threads_)
    threads.emplace_back(kv.second.get());
  return threads;
}

void DebuggedProcess::PopulateCurrentThreads() {
  for (zx_koid_t koid :
       GetChildKoids(process_.get(), ZX_INFO_PROCESS_THREADS)) {
    FXL_DCHECK(threads_.find(koid) == threads_.end());

    zx_handle_t handle;
    if (zx_object_get_child(process_.get(), koid, ZX_RIGHT_SAME_RIGHTS,
                            &handle) == ZX_OK) {
      auto added = threads_.emplace(
          koid, std::make_unique<DebuggedThread>(
                    this, zx::thread(handle), koid,
                    ThreadCreationOption::kRunningKeepRunning));
      added.first->second->SendThreadNotification();
    }
  }
}

bool DebuggedProcess::RegisterDebugState() {
  if (dl_debug_addr_)
    return true;  // Previously set.

  uintptr_t debug_addr = 0;
  if (process_.get_property(ZX_PROP_PROCESS_DEBUG_ADDR, &debug_addr,
                            sizeof(debug_addr)) != ZX_OK)
    return false;  // Can't read value.

  if (!debug_addr || debug_addr == ZX_PROCESS_DEBUG_ADDR_BREAK_ON_SET)
    return false;  // Still not set.

  dl_debug_addr_ = debug_addr;

  // TODO(brettw) register breakpoint for dynamic loads. This current code
  // only notifies for the initial set of binaries loaded by the process.
  return true;
}

void DebuggedProcess::SendModuleNotification(
    std::vector<uint64_t> paused_thread_koids) {
  // Notify the client of any libraries.
  debug_ipc::NotifyModules notify;
  notify.process_koid = koid_;
  GetModulesForProcess(process_, dl_debug_addr_, &notify.modules);
  notify.stopped_thread_koids = std::move(paused_thread_koids);

  debug_ipc::MessageWriter writer;
  debug_ipc::WriteNotifyModules(notify, &writer);
  debug_agent_->stream()->Write(writer.MessageComplete());
}

ProcessBreakpoint* DebuggedProcess::FindProcessBreakpointForAddr(
    uint64_t address) {
  auto found = breakpoints_.find(address);
  if (found == breakpoints_.end())
    return nullptr;
  return found->second.get();
}

zx_status_t DebuggedProcess::RegisterBreakpoint(Breakpoint* bp,
                                                uint64_t address) {
  auto found = breakpoints_.find(address);
  if (found == breakpoints_.end()) {
    auto process_breakpoint =
        std::make_unique<ProcessBreakpoint>(bp, this, this, address);
    zx_status_t status = process_breakpoint->Init();
    if (status != ZX_OK)
      return status;

    breakpoints_[address] = std::move(process_breakpoint);
  } else {
    found->second->RegisterBreakpoint(bp);
  }
  return ZX_OK;
}

void DebuggedProcess::UnregisterBreakpoint(Breakpoint* bp, uint64_t address) {
  auto found = breakpoints_.find(address);
  if (found == breakpoints_.end()) {
    FXL_NOTREACHED();  // Should always be found.
    return;
  }

  bool still_used = found->second->UnregisterBreakpoint(bp);
  if (!still_used) {
    for (auto& pair : threads_)
      pair.second->WillDeleteProcessBreakpoint(found->second.get());
    breakpoints_.erase(found);
  }
}

zx_status_t DebuggedProcess::RegisterWatchpoint(
    Watchpoint* wp, const debug_ipc::AddressRange& range) {
  // We should not install the same watchpoint twice.
  FXL_DCHECK(watchpoints_.find(range) != watchpoints_.end());

  auto process_wp = std::make_unique<ProcessWatchpoint>(wp, this, range);
  if (zx_status_t res = process_wp->Init(); res != ZX_OK)
    return res;

  // We let the associated Watchpoint know about this installed process wp.
  watchpoints_[range] = std::move(process_wp);
  return ZX_OK;
}

void DebuggedProcess::UnregisterWatchpoint(
    Watchpoint* wp, const debug_ipc::AddressRange& range) {
  // The process watchpoint owns the resource and will free it upon destruction.
  auto node = watchpoints_.extract(range);
  FXL_DCHECK(!node.empty());
}

void DebuggedProcess::OnProcessTerminated(zx_koid_t process_koid) {
  debug_ipc::NotifyProcess notify;
  notify.process_koid = process_koid;

  zx_info_process info;
  GetProcessInfo(process_.get(), &info);
  notify.return_code = info.return_code;

  debug_ipc::MessageWriter writer;
  debug_ipc::WriteNotifyProcess(notify, &writer);
  debug_agent_->stream()->Write(writer.MessageComplete());

  debug_agent_->RemoveDebuggedProcess(process_koid);
  // "THIS" IS NOW DELETED.
}

void DebuggedProcess::OnThreadStarting(zx_koid_t process_koid,
                                       zx_koid_t thread_koid) {
  zx::thread thread = ThreadForKoid(process_.get(), thread_koid);

  FXL_DCHECK(threads_.find(thread_koid) == threads_.end());
  ThreadCreationOption option = ThreadCreationOption::kSuspendedShouldRun;
  if (waiting_for_initial_thread_) {
    waiting_for_initial_thread_ = false;
    if (!resume_initial_thread_)
      option = ThreadCreationOption::kSuspendedKeepSuspended;
  }
  auto added = threads_.emplace(
      thread_koid, std::make_unique<DebuggedThread>(this, std::move(thread),
                                                    thread_koid, option));

  // Notify the client.
  added.first->second->SendThreadNotification();
}

void DebuggedProcess::OnThreadExiting(zx_koid_t process_koid,
                                      zx_koid_t thread_koid) {
  // Clean up our DebuggedThread object.
  auto found_thread = threads_.find(thread_koid);
  if (found_thread == threads_.end()) {
    FXL_NOTREACHED();
    return;
  }

  // The thread will currently be in a "Dying" state. For it to complete its
  // lifecycle it must be resumed.
  debug_ipc::MessageLoopTarget::Current()->ResumeFromException(
      thread_koid, found_thread->second->thread(), 0);

  threads_.erase(thread_koid);

  // Notify the client. Can't call FillThreadRecord since the thread doesn't
  // exist any more.
  debug_ipc::NotifyThread notify;
  notify.process_koid = process_koid;
  notify.record.koid = thread_koid;
  notify.record.state = debug_ipc::ThreadRecord::State::kDead;

  debug_ipc::MessageWriter writer;
  debug_ipc::WriteNotifyThread(debug_ipc::MsgHeader::Type::kNotifyThreadExiting,
                               notify, &writer);
  debug_agent_->stream()->Write(writer.MessageComplete());
}

void DebuggedProcess::OnException(zx_koid_t process_koid, zx_koid_t thread_koid,
                                  uint32_t type) {
  DebuggedThread* thread = GetThread(thread_koid);
  if (thread) {
    thread->OnException(type);
  } else {
    fprintf(stderr,
            "Exception for thread %" PRIu64 " which we don't know about.\n",
            thread_koid);
  }
}

void DebuggedProcess::OnAddressSpace(
    const debug_ipc::AddressSpaceRequest& request,
    debug_ipc::AddressSpaceReply* reply) {
  std::vector<zx_info_maps_t> map = GetProcessMaps(process_);
  if (request.address != 0u) {
    for (const auto& entry : map) {
      if (request.address < entry.base)
        continue;
      if (request.address <= (entry.base + entry.size)) {
        reply->map.push_back({entry.name, entry.base, entry.size, entry.depth});
      }
    }
    return;
  }

  size_t ix = 0;
  reply->map.resize(map.size());
  for (const auto& entry : map) {
    reply->map[ix].name = entry.name;
    reply->map[ix].base = entry.base;
    reply->map[ix].size = entry.size;
    reply->map[ix].depth = entry.depth;
    ++ix;
  }
}

void DebuggedProcess::OnModules(debug_ipc::ModulesReply* reply) {
  // Modules can only be read after the debug state is set.
  if (dl_debug_addr_)
    GetModulesForProcess(process_, dl_debug_addr_, &reply->modules);
}

void DebuggedProcess::OnSymbolTables(
    const debug_ipc::SymbolTablesRequest& request,
    debug_ipc::SymbolTablesReply* reply) {
  GetSymbolTableFromProcess(process_, request.base, request.build_id,
                            &reply->symbols);
}

void DebuggedProcess::OnWriteMemory(
    const debug_ipc::WriteMemoryRequest& request,
    debug_ipc::WriteMemoryReply* reply) {
  size_t actual = 0;
  reply->status = process_.write_memory(request.address, &request.data[0],
                                        request.data.size(), &actual);
  if (reply->status == ZX_OK && actual != request.data.size())
    reply->status = ZX_ERR_IO;  // Convert partial writes to errors.
}

void DebuggedProcess::PauseAll(std::vector<uint64_t>* paused_koids) {
  for (auto& pair : threads_) {
    if (pair.second->Pause() && paused_koids)
      paused_koids->push_back(pair.first);
  }
}

zx_status_t DebuggedProcess::ReadProcessMemory(uintptr_t address, void* buffer,
                                               size_t len, size_t* actual) {
  return process_.read_memory(address, buffer, len, actual);
}

zx_status_t DebuggedProcess::WriteProcessMemory(uintptr_t address,
                                                const void* buffer, size_t len,
                                                size_t* actual) {
  return process_.write_memory(address, buffer, len, actual);
}

}  // namespace debug_agent
