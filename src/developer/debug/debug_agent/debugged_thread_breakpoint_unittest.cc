// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/debugged_thread.h"
#include "src/developer/debug/debug_agent/local_stream_backend.h"
#include "src/developer/debug/debug_agent/mock_object_provider.h"
#include "src/developer/debug/debug_agent/mock_process.h"
#include "src/developer/debug/debug_agent/mock_process_breakpoint.h"
#include "src/developer/debug/debug_agent/object_provider.h"

namespace debug_agent {

namespace {

// Dependencies -----------------------------------------------------------------------------------

class MockArchProvider : public arch::ArchProvider {
 public:
  debug_ipc::ExceptionType DecodeExceptionType(const DebuggedThread&,
                                               uint32_t exception_type) override {
    return exception_type_;
  }

  zx_status_t ReadGeneralState(const zx::thread&, zx_thread_state_general_regs*) override {
    return ZX_OK;
  }

  zx_status_t WriteGeneralState(const zx::thread&,
                                const zx_thread_state_general_regs& regs) override {
    return ZX_OK;
  }

  zx_status_t GetInfo(const zx::thread&, zx_object_info_topic_t topic, void* buffer,
                      size_t buffer_size, size_t* actual, size_t* avail) override {
    zx_info_thread* info = reinterpret_cast<zx_info_thread*>(buffer);
    info->state = ZX_THREAD_STATE_BLOCKED_EXCEPTION;

    return ZX_OK;
  }

  uint64_t* IPInRegs(zx_thread_state_general_regs* regs) override { return &exception_addr_; }

  uint64_t BreakpointInstructionForSoftwareExceptionAddress(uint64_t exception_addr) override {
    return exception_addr;
  }

  void set_exception_addr(uint64_t addr) { exception_addr_ = addr; }
  void set_exception_type(debug_ipc::ExceptionType type) { exception_type_ = type; }

 private:
  uint64_t exception_addr_ = 0;
  debug_ipc::ExceptionType exception_type_ = debug_ipc::ExceptionType::kLast;
};

class TestProcess : public MockProcess {
 public:
  TestProcess(DebugAgent* debug_agent, zx_koid_t koid, std::string name,
              std::shared_ptr<arch::ArchProvider> arch_provider,
              std::shared_ptr<ObjectProvider> object_provider)
      : MockProcess(debug_agent, koid, std::move(name), std::move(arch_provider),
                    std::move(object_provider)) {}

  ProcessBreakpoint* FindSoftwareBreakpoint(uint64_t address) const override {
    auto it = software_breakpoints_.find(address);
    if (it == software_breakpoints_.end())
      return nullptr;
    return it->second.get();
  }

  void AppendProcessBreakpoint(Breakpoint* breakpoint, uint64_t address) {
    software_breakpoints_[address] = std::make_unique<MockProcessBreakpoint>(
        breakpoint, this, address, debug_ipc::BreakpointType::kSoftware);
  }

 private:
  std::map<uint64_t, std::unique_ptr<MockProcessBreakpoint>> software_breakpoints_;
};

class TestStreamBackend : public LocalStreamBackend {
 public:
  void HandleNotifyException(debug_ipc::NotifyException exception) override {
    exceptions_.push_back(std::move(exception));
  }

  const std::vector<debug_ipc::NotifyException>& exceptions() const { return exceptions_; }

 private:
  std::vector<debug_ipc::NotifyException> exceptions_;
};

// Helpers -----------------------------------------------------------------------------------------

struct TestContext {
  std::shared_ptr<MockArchProvider> arch_provider;
  std::shared_ptr<MockObjectProvider> object_provider;

  std::unique_ptr<DebugAgent> debug_agent;
  std::unique_ptr<TestStreamBackend> backend;
};

TestContext CreateTestContext() {
  TestContext context;

  // Mock the system.
  context.arch_provider = std::make_shared<MockArchProvider>();
  context.object_provider = CreateDefaultMockObjectProvider();

  // Create the debug agent.
  SystemProviders providers;
  providers.arch_provider = context.arch_provider;
  providers.object_provider = context.object_provider;
  context.debug_agent = std::make_unique<DebugAgent>(nullptr, std::move(providers));

  // Create the connection to the debug agent.
  context.backend = std::make_unique<TestStreamBackend>();
  context.debug_agent->Connect(&context.backend->stream());

  return context;
}

std::pair<const MockProcessObject*, const MockThreadObject*> GetProcessThread(
    const MockObjectProvider& object_provider, const std::string& process_name,
    const std::string& thread_name) {
  auto* process = object_provider.ProcessByName(process_name);
  FXL_DCHECK(process);
  auto* thread = process->GetThread(thread_name);
  FXL_DCHECK(thread);

  return {process, thread};
}

// Tests -------------------------------------------------------------------------------------------

TEST(DebuggedThreadBreakpoint, NormalException) {
  TestContext context = CreateTestContext();

  // Create a process from our mocked object hierarchy.
  auto [proc_object, thread_object] =
      GetProcessThread(*context.object_provider, "job121-p2", "second-thread");
  TestProcess process(context.debug_agent.get(), proc_object->koid, proc_object->name,
                      context.arch_provider, context.object_provider);

  // Create the thread that will be on an exception.
  DebuggedThread::CreateInfo create_info = {};
  create_info.process = &process;
  create_info.koid = thread_object->koid;
  create_info.handle = thread_object->GetHandle();
  create_info.arch_provider = context.arch_provider;
  create_info.object_provider = context.object_provider;
  DebuggedThread thread(context.debug_agent.get(), std::move(create_info));

  // Set the exception information the arch provider is going to return.
  constexpr uint64_t kAddress = 0xdeadbeef;
  context.arch_provider->set_exception_addr(kAddress);
  context.arch_provider->set_exception_type(debug_ipc::ExceptionType::kPageFault);

  // Trigger the exception.
  zx_exception_info exception_info = {};
  exception_info.pid = proc_object->koid;
  exception_info.tid = thread_object->koid;
  exception_info.type = ZX_EXCP_FATAL_PAGE_FAULT;
  thread.OnException(zx::exception(), exception_info);

  // We should've received an exception notification.
  ASSERT_EQ(context.backend->exceptions().size(), 1u);
  {
    EXPECT_EQ(context.backend->exceptions()[0].type, debug_ipc::ExceptionType::kPageFault);
    EXPECT_EQ(context.backend->exceptions()[0].hit_breakpoints.size(), 0u);

    auto& thread_record = context.backend->exceptions()[0].thread;
    EXPECT_EQ(thread_record.process_koid, proc_object->koid);
    EXPECT_EQ(thread_record.thread_koid, thread_object->koid);
    EXPECT_EQ(thread_record.state, debug_ipc::ThreadRecord::State::kBlocked);
    EXPECT_EQ(thread_record.blocked_reason, debug_ipc::ThreadRecord::BlockedReason::kException);
    EXPECT_EQ(thread_record.stack_amount, debug_ipc::ThreadRecord::StackAmount::kMinimal);
  }
}

}  // namespace
}  // namespace debug_agent
