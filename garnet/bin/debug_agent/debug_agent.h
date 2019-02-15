// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_DEBUG_AGENT_DEBUG_AGENT_H_
#define GARNET_BIN_DEBUG_AGENT_DEBUG_AGENT_H_

#include <map>
#include <memory>

#include <zircon/types.h>

#include "garnet/bin/debug_agent/breakpoint.h"
#include "garnet/bin/debug_agent/debugged_job.h"
#include "garnet/bin/debug_agent/debugged_process.h"
#include "garnet/bin/debug_agent/remote_api.h"
#include "garnet/bin/debug_agent/watchpoint.h"
#include "garnet/lib/debug_ipc/helper/stream_buffer.h"
#include "lib/fxl/macros.h"
#include "lib/sys/cpp/service_directory.h"

namespace debug_agent {

// Main state and control for the debug agent.
class DebugAgent : public RemoteAPI,
                   public ProcessStartHandler,
                   public Breakpoint::ProcessDelegate,
                   public Watchpoint::ProcessDelegate {
 public:
  // A MessageLoopZircon should already be set up on the current thread.
  //
  // The stream must outlive this class. It will be used to send data to the
  // client. It will not be read (that's the job of the provider of the
  // RemoteAPI).
  explicit DebugAgent(debug_ipc::StreamBuffer* stream,
                      std::shared_ptr<sys::ServiceDirectory> services);
  ~DebugAgent();

  debug_ipc::StreamBuffer* stream() { return stream_; }

  void RemoveDebuggedProcess(zx_koid_t process_koid);

  void RemoveDebuggedJob(zx_koid_t job_koid);

  void RemoveBreakpoint(uint32_t breakpoint_id);

  void OnProcessStart(zx::process process) override;

  bool should_quit() const { return should_quit_; }

 private:
  // RemoteAPI implementation.
  void OnHello(const debug_ipc::HelloRequest& request,
               debug_ipc::HelloReply* reply) override;
  void OnLaunch(const debug_ipc::LaunchRequest& request,
                debug_ipc::LaunchReply* reply) override;
  void OnKill(const debug_ipc::KillRequest& request,
              debug_ipc::KillReply* reply) override;
  void OnAttach(std::vector<char> serialized) override;
  void OnDetach(const debug_ipc::DetachRequest& request,
                debug_ipc::DetachReply* reply) override;
  void OnPause(const debug_ipc::PauseRequest& request,
               debug_ipc::PauseReply* reply) override;
  void OnQuitAgent(const debug_ipc::QuitAgentRequest& request,
                   debug_ipc::QuitAgentReply* reply) override;
  void OnResume(const debug_ipc::ResumeRequest& request,
                debug_ipc::ResumeReply* reply) override;
  void OnModules(const debug_ipc::ModulesRequest& request,
                 debug_ipc::ModulesReply* reply) override;
  void OnProcessTree(const debug_ipc::ProcessTreeRequest& request,
                     debug_ipc::ProcessTreeReply* reply) override;
  void OnThreads(const debug_ipc::ThreadsRequest& request,
                 debug_ipc::ThreadsReply* reply) override;
  void OnReadMemory(const debug_ipc::ReadMemoryRequest& request,
                    debug_ipc::ReadMemoryReply* reply) override;
  void OnReadRegisters(const debug_ipc::ReadRegistersRequest& request,
                       debug_ipc::ReadRegistersReply* reply) override;
  void OnWriteRegisters(const debug_ipc::WriteRegistersRequest& request,
                        debug_ipc::WriteRegistersReply* reply) override;
  void OnAddOrChangeBreakpoint(
      const debug_ipc::AddOrChangeBreakpointRequest& request,
      debug_ipc::AddOrChangeBreakpointReply* reply) override;
  void OnRemoveBreakpoint(const debug_ipc::RemoveBreakpointRequest& request,
                          debug_ipc::RemoveBreakpointReply* reply) override;
  void OnThreadStatus(const debug_ipc::ThreadStatusRequest& request,
                      debug_ipc::ThreadStatusReply* reply) override;
  void OnAddressSpace(const debug_ipc::AddressSpaceRequest& request,
                      debug_ipc::AddressSpaceReply* reply) override;
  void OnJobFilter(const debug_ipc::JobFilterRequest& request,
                   debug_ipc::JobFilterReply* reply) override;
  void OnWriteMemory(const debug_ipc::WriteMemoryRequest& request,
                     debug_ipc::WriteMemoryReply* reply) override;
  void OnSymbolTables(const debug_ipc::SymbolTablesRequest& request,
                      debug_ipc::SymbolTablesReply* reply) override;

  // Breakpoint::ProcessDelegate implementation --------------------------------

  zx_status_t RegisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid,
                                 uint64_t address) override;
  void UnregisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid,
                            uint64_t address) override;

  // Watchpoint::ProcessDelegate implementation --------------------------------

  zx_status_t RegisterWatchpoint(Watchpoint*, zx_koid_t process_koid,
                                 const debug_ipc::AddressRange&) override;
  void UnregisterWatchpoint(Watchpoint*, zx_koid_t process_koid,
                            const debug_ipc::AddressRange&) override;

  // Job/Process/Thread Management ---------------------------------------------

  // Returns the debugged process/thread for the given koid(s) or null if not
  // found.
  DebuggedJob* AddDebuggedJob(zx_koid_t job_koid, zx::job zx_job);
  DebuggedProcess* GetDebuggedProcess(zx_koid_t koid);
  DebuggedJob* GetDebuggedJob(zx_koid_t koid);
  DebuggedThread* GetDebuggedThread(zx_koid_t process_koid,
                                    zx_koid_t thread_koid);

  // Returns a pointer to the newly created object.
  DebuggedProcess* AddDebuggedProcess(zx_koid_t process_koid,
                                      zx::process zx_proc,
                                      bool resume_initial_thread);

  void LaunchProcess(const debug_ipc::LaunchRequest&, debug_ipc::LaunchReply*);
  void LaunchComponent(const debug_ipc::LaunchRequest&,
                       debug_ipc::LaunchReply*);

  debug_ipc::StreamBuffer* stream_;

  std::shared_ptr<sys::ServiceDirectory> services_;

  std::map<zx_koid_t, std::unique_ptr<DebuggedProcess>> procs_;

  std::map<zx_koid_t, std::unique_ptr<DebuggedJob>> jobs_;

  std::map<uint32_t, Breakpoint> breakpoints_;
  std::map<uint32_t, Watchpoint> watchpoints_;

  // Whether the debug agent should exit.
  // The main reason for this is receiving a QuitNow message.
  bool should_quit_ = false;

  // Normally the debug agent would be attached to the base component and give
  // the client the koid. This is a job koid needed to be able to create an
  // invisible filter to catch the newly started component.
  // TODO(donosoc): Hopefully we could get the created job for the component
  //                so we can only filter on that.
  zx_koid_t component_root_job_koid_ = 0;

  FXL_DISALLOW_COPY_AND_ASSIGN(DebugAgent);
};

}  // namespace debug_agent

#endif  // GARNET_BIN_DEBUG_AGENT_DEBUG_AGENT_H_
