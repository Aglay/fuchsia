// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/debugged_thread.h"
#include "src/developer/debug/debug_agent/hardware_breakpoint.h"
#include "src/developer/debug/debug_agent/local_stream_backend.h"
#include "src/developer/debug/debug_agent/mock_object_provider.h"
#include "src/developer/debug/debug_agent/mock_process.h"
#include "src/developer/debug/debug_agent/mock_process_breakpoint.h"
#include "src/developer/debug/debug_agent/mock_system_interface.h"
#include "src/developer/debug/debug_agent/mock_thread_exception.h"
#include "src/developer/debug/debug_agent/mock_thread_handle.h"
#include "src/developer/debug/debug_agent/object_provider.h"
#include "src/developer/debug/debug_agent/software_breakpoint.h"
#include "src/developer/debug/debug_agent/watchpoint.h"

namespace debug_agent {

namespace {

// Dependencies -----------------------------------------------------------------------------------

class TestProcess : public MockProcess {
 public:
  TestProcess(DebugAgent* debug_agent, zx_koid_t koid, std::string name,
              std::shared_ptr<ObjectProvider> object_provider)
      : MockProcess(debug_agent, koid, std::move(name), std::move(object_provider)) {}

  SoftwareBreakpoint* FindSoftwareBreakpoint(uint64_t address) const override {
    auto it = software_breakpoints_.find(address);
    if (it == software_breakpoints_.end())
      return nullptr;
    return it->second.get();
  }

  HardwareBreakpoint* FindHardwareBreakpoint(uint64_t address) const override {
    auto it = hardware_breakpoints_.find(address);
    if (it == hardware_breakpoints_.end())
      return nullptr;
    return it->second.get();
  }

  Watchpoint* FindWatchpoint(const debug_ipc::AddressRange& range) const override {
    for (auto& [r, watchpoint] : watchpoints_) {
      if (r.Contains(range))
        return watchpoint.get();
    }

    return nullptr;
  }

  void AppendSofwareBreakpoint(Breakpoint* breakpoint, uint64_t address) {
    software_breakpoints_[address] =
        std::make_unique<MockSoftwareBreakpoint>(breakpoint, this, address);
  }

  void AppendHardwareBreakpoint(Breakpoint* breakpoint, uint64_t address) {
    hardware_breakpoints_[address] =
        std::make_unique<MockHardwareBreakpoint>(breakpoint, this, address);
  }

  void AppendWatchpoint(Breakpoint* breakpoint, debug_ipc::AddressRange range) {
    watchpoints_[range] =
        std::make_unique<Watchpoint>(debug_ipc::BreakpointType::kWrite, breakpoint, this, range);
  }

 private:
  std::map<uint64_t, std::unique_ptr<MockSoftwareBreakpoint>> software_breakpoints_;
  std::map<uint64_t, std::unique_ptr<MockHardwareBreakpoint>> hardware_breakpoints_;
  WatchpointMap watchpoints_;
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

class MockProcessDelegate : public Breakpoint::ProcessDelegate {
 public:
  zx_status_t RegisterBreakpoint(Breakpoint*, zx_koid_t, uint64_t) override { return ZX_OK; }
  void UnregisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid, uint64_t address) override {}

  zx_status_t RegisterWatchpoint(Breakpoint*, zx_koid_t, const debug_ipc::AddressRange&) override {
    return ZX_OK;
  }
  void UnregisterWatchpoint(Breakpoint*, zx_koid_t, const debug_ipc::AddressRange&) override {}
};

// Helpers -----------------------------------------------------------------------------------------

struct TestContext {
  std::shared_ptr<LimboProvider> limbo_provider;
  std::shared_ptr<MockObjectProvider> object_provider;

  std::unique_ptr<DebugAgent> debug_agent;
  std::unique_ptr<TestStreamBackend> backend;
};

TestContext CreateTestContext() {
  TestContext context;

  // Mock the system.
  context.limbo_provider = std::make_shared<LimboProvider>(nullptr);
  context.object_provider = CreateDefaultMockObjectProvider();

  // Create the debug agent.
  SystemProviders providers;
  providers.limbo_provider = context.limbo_provider;
  providers.object_provider = context.object_provider;
  context.debug_agent = std::make_unique<DebugAgent>(
      std::make_unique<MockSystemInterface>(MockJobHandle(1)), nullptr, std::move(providers));

  // Create the connection to the debug agent.
  context.backend = std::make_unique<TestStreamBackend>();
  context.debug_agent->Connect(&context.backend->stream());

  return context;
}

std::pair<const MockProcessObject*, const MockThreadObject*> GetProcessThread(
    const MockObjectProvider& object_provider, const std::string& process_name,
    const std::string& thread_name) {
  auto* process = object_provider.ProcessByName(process_name);
  FX_DCHECK(process);
  auto* thread = process->GetThread(thread_name);
  FX_DCHECK(thread);

  return {process, thread};
}

// If |thread| is null, it means a process-wide breakpoint.
debug_ipc::ProcessBreakpointSettings CreateLocation(zx_koid_t process_koid, zx_koid_t thread_koid,
                                                    uint64_t address) {
  debug_ipc::ProcessBreakpointSettings location = {};
  location.process_koid = process_koid;
  location.thread_koid = thread_koid;
  location.address = address;

  return location;
}

// If |thread| is null, it means a process-wide breakpoint.
debug_ipc::ProcessBreakpointSettings CreateLocation(zx_koid_t process_koid, zx_koid_t thread_koid,
                                                    debug_ipc::AddressRange range) {
  debug_ipc::ProcessBreakpointSettings location = {};
  location.process_koid = process_koid;
  location.thread_koid = thread_koid;
  location.address_range = range;

  return location;
}

// Tests -------------------------------------------------------------------------------------------

TEST(DebuggedThreadBreakpoint, NormalException) {
  TestContext context = CreateTestContext();

  // Create a process from our mocked object hierarchy.
  auto [proc_object, thread_object] =
      GetProcessThread(*context.object_provider, "job121-p2", "second-thread");
  TestProcess process(context.debug_agent.get(), proc_object->koid, proc_object->name,
                      context.object_provider);

  auto owning_thread_handle = std::make_unique<MockThreadHandle>(thread_object->koid);
  MockThreadHandle* mock_thread_handle = owning_thread_handle.get();

  // Create the thread that will be on an exception.
  DebuggedThread::CreateInfo create_info = {};
  create_info.process = &process;
  create_info.koid = thread_object->koid;
  create_info.handle = std::move(owning_thread_handle);
  DebuggedThread thread(context.debug_agent.get(), std::move(create_info));

  // Set the exception information the arch provider is going to return.
  constexpr uint64_t kAddress = 0xdeadbeef;

  // The current thread address should agree with the exception.
  GeneralRegisters regs;
  regs.set_ip(kAddress);
  mock_thread_handle->SetGeneralRegisters(regs);
  mock_thread_handle->set_state(
      ThreadHandle::State(debug_ipc::ThreadRecord::BlockedReason::kException));

  // Trigger the exception.
  thread.OnException(std::make_unique<MockThreadException>(thread_object->koid,
                                                           debug_ipc::ExceptionType::kPageFault));

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

TEST(DebuggedThreadBreakpoint, SWBreakpoint) {
  TestContext context = CreateTestContext();

  // Create a process from our mocked object hierarchy.
  auto [proc_object, thread_object] =
      GetProcessThread(*context.object_provider, "job121-p2", "second-thread");
  TestProcess process(context.debug_agent.get(), proc_object->koid, proc_object->name,
                      context.object_provider);

  auto owning_thread_handle = std::make_unique<MockThreadHandle>(thread_object->koid);
  MockThreadHandle* mock_thread_handle = owning_thread_handle.get();

  // Create the thread that will be on an exception.
  DebuggedThread::CreateInfo create_info = {};
  create_info.process = &process;
  create_info.koid = thread_object->koid;
  create_info.handle = std::move(owning_thread_handle);
  DebuggedThread thread(context.debug_agent.get(), std::move(create_info));

  // Set the exception information the arch provider is going to return. Some architectures like
  // x64 will issue the exception on the following address, so we need to back-compute it.
  constexpr uint64_t kBreakpointAddress = 0xdeadbeef;
  const uint64_t kExceptionOffset =
      kBreakpointAddress -
      arch::BreakpointInstructionForSoftwareExceptionAddress(kBreakpointAddress);
  const uint64_t kExceptionAddress = kBreakpointAddress + kExceptionOffset;

  // The current thread address should agree with the exception.
  GeneralRegisters regs;
  regs.set_ip(kExceptionAddress);
  mock_thread_handle->SetGeneralRegisters(regs);
  mock_thread_handle->set_state(
      ThreadHandle::State(debug_ipc::ThreadRecord::BlockedReason::kException));

  // Trigger the exception.
  thread.OnException(std::make_unique<MockThreadException>(thread_object->koid,
                                                           debug_ipc::ExceptionType::kSoftware));

  // We should've received an exception notification.
  ASSERT_EQ(context.backend->exceptions().size(), 1u);
  {
    auto& exception = context.backend->exceptions()[0];

    EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kSoftware)
        << debug_ipc::ExceptionTypeToString(exception.type);
    EXPECT_EQ(exception.hit_breakpoints.size(), 0u);

    auto& thread_record = exception.thread;
    EXPECT_EQ(thread_record.process_koid, proc_object->koid);
    EXPECT_EQ(thread_record.thread_koid, thread_object->koid);
    EXPECT_EQ(thread_record.state, debug_ipc::ThreadRecord::State::kBlocked);
    EXPECT_EQ(thread_record.blocked_reason, debug_ipc::ThreadRecord::BlockedReason::kException);
    EXPECT_EQ(thread_record.stack_amount, debug_ipc::ThreadRecord::StackAmount::kMinimal);
  }

  // Add a breakpoint on that address.
  constexpr uint32_t kBreakpointId = 1000;
  MockProcessDelegate process_delegate;
  auto breakpoint = std::make_unique<Breakpoint>(&process_delegate);
  debug_ipc::BreakpointSettings settings = {};
  settings.id = kBreakpointId;
  settings.type = debug_ipc::BreakpointType::kSoftware;
  settings.locations.push_back(CreateLocation(proc_object->koid, 0, kBreakpointAddress));
  breakpoint->SetSettings(settings);

  process.AppendSofwareBreakpoint(breakpoint.get(), kBreakpointAddress);

  // Throw the same breakpoint exception.
  thread.OnException(std::make_unique<MockThreadException>(thread_object->koid,
                                                           debug_ipc::ExceptionType::kSoftware));

  // We should've received an exception notification with hit breakpoints.
  ASSERT_EQ(context.backend->exceptions().size(), 2u);
  {
    auto& exception = context.backend->exceptions()[1];

    EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kSoftware)
        << debug_ipc::ExceptionTypeToString(exception.type);
    ASSERT_EQ(exception.hit_breakpoints.size(), 1u);
    EXPECT_EQ(exception.hit_breakpoints[0].id, breakpoint->stats().id);
    EXPECT_EQ(breakpoint->stats().hit_count, 1u);

    auto& thread_record = exception.thread;
    EXPECT_EQ(thread_record.process_koid, proc_object->koid);
    EXPECT_EQ(thread_record.thread_koid, thread_object->koid);
    EXPECT_EQ(thread_record.state, debug_ipc::ThreadRecord::State::kBlocked);
    EXPECT_EQ(thread_record.blocked_reason, debug_ipc::ThreadRecord::BlockedReason::kException);
    EXPECT_EQ(thread_record.stack_amount, debug_ipc::ThreadRecord::StackAmount::kMinimal);
  }
}

TEST(DebuggedThreadBreakpoint, HWBreakpoint) {
  TestContext context = CreateTestContext();

  // Create a process from our mocked object hierarchy.
  auto [proc_object, thread_object] =
      GetProcessThread(*context.object_provider, "job121-p2", "second-thread");
  TestProcess process(context.debug_agent.get(), proc_object->koid, proc_object->name,
                      context.object_provider);

  auto owning_thread_handle = std::make_unique<MockThreadHandle>(thread_object->koid);
  MockThreadHandle* mock_thread_handle = owning_thread_handle.get();

  // Create the thread that will be on an exception.
  DebuggedThread::CreateInfo create_info = {};
  create_info.process = &process;
  create_info.koid = thread_object->koid;
  create_info.handle = std::move(owning_thread_handle);
  DebuggedThread thread(context.debug_agent.get(), std::move(create_info));

  // Set the exception information the arch provider is going to return.
  constexpr uint64_t kAddress = 0xdeadbeef;

  // The current thread address should agree with the exception.
  GeneralRegisters regs;
  regs.set_ip(kAddress);
  mock_thread_handle->SetGeneralRegisters(regs);
  mock_thread_handle->set_state(
      ThreadHandle::State(debug_ipc::ThreadRecord::BlockedReason::kException));

  // Add a breakpoint on that address.
  constexpr uint32_t kBreakpointId = 1000;
  MockProcessDelegate process_delegate;
  auto breakpoint = std::make_unique<Breakpoint>(&process_delegate);
  debug_ipc::BreakpointSettings settings = {};
  settings.id = kBreakpointId;
  settings.type = debug_ipc::BreakpointType::kHardware;
  settings.locations.push_back(CreateLocation(proc_object->koid, 0, kAddress));
  breakpoint->SetSettings(settings);

  process.AppendHardwareBreakpoint(breakpoint.get(), kAddress);

  // Trigger the exception.
  thread.OnException(std::make_unique<MockThreadException>(thread_object->koid,
                                                           debug_ipc::ExceptionType::kHardware));

  // We should've received an exception notification.
  ASSERT_EQ(context.backend->exceptions().size(), 1u);
  {
    auto& exception = context.backend->exceptions()[0];

    EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kHardware)
        << debug_ipc::ExceptionTypeToString(exception.type);
    EXPECT_EQ(exception.hit_breakpoints.size(), 1u);
    EXPECT_EQ(exception.hit_breakpoints[0].id, breakpoint->stats().id);
    EXPECT_EQ(breakpoint->stats().hit_count, 1u);

    auto& thread_record = exception.thread;
    EXPECT_EQ(thread_record.process_koid, proc_object->koid);
    EXPECT_EQ(thread_record.thread_koid, thread_object->koid);
    EXPECT_EQ(thread_record.state, debug_ipc::ThreadRecord::State::kBlocked);
    EXPECT_EQ(thread_record.blocked_reason, debug_ipc::ThreadRecord::BlockedReason::kException);
    EXPECT_EQ(thread_record.stack_amount, debug_ipc::ThreadRecord::StackAmount::kMinimal);
  }
}

TEST(DebuggedThreadBreakpoint, Watchpoint) {
  constexpr uint64_t kWatchpointLength = 8;

  TestContext context = CreateTestContext();

  // Create a process from our mocked object hierarchy.
  auto [proc_object, thread_object] =
      GetProcessThread(*context.object_provider, "job121-p2", "second-thread");
  TestProcess process(context.debug_agent.get(), proc_object->koid, proc_object->name,
                      context.object_provider);

  auto owning_thread_handle = std::make_unique<MockThreadHandle>(thread_object->koid);
  MockThreadHandle* mock_thread_handle = owning_thread_handle.get();

  // Create the thread that will be on an exception.
  DebuggedThread::CreateInfo create_info = {};
  create_info.process = &process;
  create_info.koid = thread_object->koid;
  create_info.handle = std::move(owning_thread_handle);
  DebuggedThread thread(context.debug_agent.get(), std::move(create_info));

  // Add a watchpoint.
  const debug_ipc::AddressRange kRange = {0x1000, 0x1000 + kWatchpointLength};
  MockProcessDelegate process_delegate;
  Breakpoint breakpoint(&process_delegate);

  constexpr uint32_t kBreakpointId = 1000;
  debug_ipc::BreakpointSettings settings = {};
  settings.id = kBreakpointId;
  settings.type = debug_ipc::BreakpointType::kWrite;
  settings.locations.push_back(CreateLocation(proc_object->koid, 0, kRange));
  breakpoint.SetSettings(settings);

  process.AppendWatchpoint(&breakpoint, kRange);

  // Set the exception information in the debug registers to return. This should indicate the
  // watchpoint that was set up, and that the watchpoing was triggered.
  const uint64_t kAddress = kRange.begin();
  DebugRegisters debug_regs;
  auto set_result = debug_regs.SetWatchpoint(debug_ipc::BreakpointType::kWrite, kRange, 4);
  ASSERT_TRUE(set_result);
  debug_regs.SetForHitWatchpoint(set_result->slot);

  // The current thread address should agree with the exception.
  GeneralRegisters regs;
  regs.set_ip(kAddress);
  mock_thread_handle->SetGeneralRegisters(regs);
  mock_thread_handle->SetDebugRegisters(debug_regs);
  mock_thread_handle->set_state(
      ThreadHandle::State(debug_ipc::ThreadRecord::BlockedReason::kException));

  // Trigger the exception.
  thread.OnException(std::make_unique<MockThreadException>(thread_object->koid,
                                                           debug_ipc::ExceptionType::kWatchpoint));

  // We should've received an exception notification.
  {
    auto& exception = context.backend->exceptions()[0];

    EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kWatchpoint)
        << debug_ipc::ExceptionTypeToString(exception.type);
    ASSERT_EQ(exception.hit_breakpoints.size(), 1u);
    EXPECT_EQ(exception.hit_breakpoints[0].id, breakpoint.stats().id);
    EXPECT_EQ(breakpoint.stats().hit_count, 1u);

    auto& thread_record = exception.thread;
    EXPECT_EQ(thread_record.process_koid, proc_object->koid);
    EXPECT_EQ(thread_record.thread_koid, thread_object->koid);
    EXPECT_EQ(thread_record.state, debug_ipc::ThreadRecord::State::kBlocked);
    EXPECT_EQ(thread_record.blocked_reason, debug_ipc::ThreadRecord::BlockedReason::kException);
    EXPECT_EQ(thread_record.stack_amount, debug_ipc::ThreadRecord::StackAmount::kMinimal);
  }
}

}  // namespace
}  // namespace debug_agent
