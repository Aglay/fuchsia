// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "interception_workflow.h"

#include <thread>
#undef __TA_REQUIRES
#include <zircon/fidl.h>

#include "gtest/gtest.h"
#include "src/developer/debug/ipc/protocol.h"
#include "src/developer/debug/zxdb/client/mock_remote_api.h"
#include "src/developer/debug/zxdb/client/remote_api_test.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"

namespace fidlcat {

// This class encapsulates the data needed for the zx_channel tests.
// TODO: This is obviously not extensible to more than one test.
class DataForZxChannelTest {
 public:
  DataForZxChannelTest(debug_ipc::Arch arch) : arch_(arch) {
    header_.txid = kTxId;
    header_.reserved0 = kReserved;
    header_.flags = kFlags;
    header_.ordinal = kOrdinal;

    // Fill out the stack with the expected values for zx_channel_read (which
    // will be ignored / irrelevant for zx_channel_write).
    memset(stack_, 0, sizeof(stack_));
    uint64_t* stack_ptr = reinterpret_cast<uint64_t*>(stack_);
    stack_ptr[1] = kActualBytesPtr;
    stack_ptr[2] = kActualHandlesPtr;

    current_stack_ptr_ = kStackPointer;
    current_symbol_address_ = 0x0;
  }

  const uint8_t* data() const {
    return reinterpret_cast<const uint8_t*>(&header_);
  }

  size_t num_bytes() const { return sizeof(header_); }

  const zx_handle_t* handles() const { return handles_; }

  size_t num_handles() const { return sizeof(handles_) / sizeof(handles_[0]); }

  uint64_t current_stack_ptr() { return current_stack_ptr_; }

  void SetCurrentAddress(uint64_t address) {
    current_symbol_address_ = address;
  }

  int64_t zx_channel_read_result() const { return zx_channel_read_result_; }
  void set_zx_channel_read_result(int64_t zx_channel_read_result) {
    zx_channel_read_result_ = zx_channel_read_result;
  }

  // read_has_handles controls whether the zx_channel_read parameters
  // passed NULL for actual_handles.
  bool read_has_handles() { return read_has_handles_; }
  void set_read_has_handles(int64_t read_has_handles) {
    read_has_handles_ = read_has_handles;
    if (!read_has_handles) {
      uint64_t* stack_ptr = reinterpret_cast<uint64_t*>(stack_);
      stack_ptr[2] = 0;
    }
  }

  // read_has_bytes controls whether the zx_channel_read parameters
  // passed NULL for actual_bytes.
  bool read_has_bytes() { return read_has_bytes_; }
  void set_read_has_bytes(int64_t read_has_bytes) {
    read_has_bytes_ = read_has_bytes;
    if (!read_has_bytes) {
      uint64_t* stack_ptr = reinterpret_cast<uint64_t*>(stack_);
      stack_ptr[1] = 0;
    }
  }

  fxl::RefPtr<zxdb::SystemSymbols::ModuleRef> GetModuleRef(
      zxdb::Session* session) {
    // Create a module with zx_channel_write and zx_channel_read
    std::unique_ptr<zxdb::MockModuleSymbols> module =
        std::make_unique<zxdb::MockModuleSymbols>("zx.so");
    module->AddSymbolLocations(
        zx_channel_write_name_,
        {zxdb::Location(zxdb::Location::State::kSymbolized,
                        kWriteElfSymbolAddress)});
    module->AddSymbolLocations(
        zx_channel_read_name_,
        {zxdb::Location(zxdb::Location::State::kSymbolized,
                        kReadElfSymbolAddress)});

    return session->system().GetSymbols()->InjectModuleForTesting(
        kElfSymbolBuildID, std::move(module));
  }

  void PopulateModules(std::vector<debug_ipc::Module>& modules) {
    const uint64_t kModuleBase = 0x1000000;
    debug_ipc::Module load;
    load.name = "test";
    load.base = kModuleBase;
    load.build_id = kElfSymbolBuildID;
    modules.push_back(load);
  }

  void PopulateMemoryBlockForAddress(uint64_t address, uint64_t size,
                                     debug_ipc::MemoryBlock& block) {
    block.address = address;
    block.size = size;
    block.valid = true;
    switch (address) {
      case kBytesAddress: {
        const uint8_t* bytes = data();
        std::copy(bytes, bytes + num_bytes(), std::back_inserter(block.data));
        break;
      }
      case kHandlesAddress: {
        const zx_handle_t* h = handles();
        std::copy(reinterpret_cast<const uint8_t*>(h),
                  reinterpret_cast<const uint8_t*>(h + num_handles()),
                  std::back_inserter(block.data));
        break;
      }
      case kStackPointer + sizeof(uint64_t): {
        // Should only be requested for zx_channel_read.  Having this
        // available for zx_channel_write should not affect behavior, though.
        std::copy(stack_ + sizeof(uint64_t), stack_ + sizeof(stack_),
                  std::back_inserter(block.data));
        break;
      }
      case kActualBytesPtr: {
        uint32_t byte_count = num_bytes();
        uint8_t* num_bytes_ptr = reinterpret_cast<uint8_t*>(&byte_count);
        std::copy(num_bytes_ptr, num_bytes_ptr + sizeof(byte_count),
                  std::back_inserter(block.data));
        break;
      }
      case kActualHandlesPtr: {
        uint32_t handle_count = num_handles();
        uint8_t* num_handles_ptr = reinterpret_cast<uint8_t*>(&handle_count);
        std::copy(num_handles_ptr, num_handles_ptr + sizeof(handle_count),
                  std::back_inserter(block.data));
        break;
      }
      default:
        FXL_NOTREACHED() << "Unknown memory address requested.";
    }
    FXL_DCHECK(size == block.data.size())
        << "expected size: " << size
        << " and actual size: " << block.data.size();
  }

  void Populate32BitRegister(debug_ipc::RegisterID register_id, uint32_t value,
                             std::vector<debug_ipc::Register>* registers) {
    Populate64BitRegister(register_id, value, registers);
  }

  void Populate64BitRegister(debug_ipc::RegisterID register_id, uint64_t value,
                             std::vector<debug_ipc::Register>* registers) {
    debug_ipc::Register& reg = registers->emplace_back();
    reg.id = register_id;
    for (int i = 0; i < 64; i += 8) {
      reg.data.push_back((value >> i) & 0xff);
    }
  }

  void PopulateRegisters(debug_ipc::RegisterCategory& category) {
    category.type = debug_ipc::RegisterCategory::Type::kGeneral;
    std::vector<debug_ipc::Register>& registers = category.registers;
    if (current_symbol_address_ == kWriteElfSymbolAddress) {
      if (arch_ == debug_ipc::Arch::kArm64) {
        // zx_handle_t handle
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x0, 0xcefa1db0,
                              &registers);
        // uint32_t options
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x1, 0, &registers);
        // bytes_address
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x2, kBytesAddress,
                              &registers);
        // num_bytes
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x3, num_bytes(),
                              &registers);
        // handles_address
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x4, kHandlesAddress,
                              &registers);
        // num_handles
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x5, num_handles(),
                              &registers);
        // stack pointer
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_sp,
                              current_stack_ptr_, &registers);
        // link register needs to be non-zero
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_lr, 0x1010101,
                              &registers);
      } else if (arch_ == debug_ipc::Arch::kX64) {
        // zx_handle_t handle
        Populate32BitRegister(debug_ipc::RegisterID::kX64_rdi, 0xcefa1db0,
                              &registers);
        // uint32_t options
        Populate32BitRegister(debug_ipc::RegisterID::kX64_rsi, 0, &registers);
        // bytes_address
        Populate64BitRegister(debug_ipc::RegisterID::kX64_rdx, kBytesAddress,
                              &registers);
        // num_bytes
        Populate32BitRegister(debug_ipc::RegisterID::kX64_rcx, num_bytes(),
                              &registers);
        // handles_address
        Populate64BitRegister(debug_ipc::RegisterID::kX64_r8, kHandlesAddress,
                              &registers);
        // num_handles
        Populate32BitRegister(debug_ipc::RegisterID::kX64_r9, num_handles(),
                              &registers);
        // stack pointer
        Populate64BitRegister(debug_ipc::RegisterID::kX64_rsp,
                              current_stack_ptr_, &registers);
      }
    } else if (current_symbol_address_ == kReadElfSymbolAddress) {
      if (arch_ == debug_ipc::Arch::kArm64) {
        if (first_register_read_) {
          // zx_handle_t handle
          Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x0, 0xcefa1db0,
                                &registers);
        } else {
          // return value for zx_channel_read
          Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x0,
                                zx_channel_read_result_, &registers);
        }
        first_register_read_ = false;
        // uint32_t options
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x1, 0, &registers);
        // bytes_address
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x2, kBytesAddress,
                              &registers);
        // handles_address
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x3, kHandlesAddress,
                              &registers);
        // num_bytes
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x4, num_bytes(),
                              &registers);
        // num_handles
        Populate32BitRegister(debug_ipc::RegisterID::kARMv8_x5, num_handles(),
                              &registers);
        // output num_bytes address
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x6,
                              read_has_bytes_ ? kActualBytesPtr : 0,
                              &registers);
        // output num_handles address
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_x7,
                              read_has_handles_ ? kActualHandlesPtr : 0,
                              &registers);
        // stack pointer
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_sp,
                              current_stack_ptr_, &registers);
        // link register needs to be non-zero
        Populate64BitRegister(debug_ipc::RegisterID::kARMv8_lr, 0x1010101,
                              &registers);
      } else if (arch_ == debug_ipc::Arch::kX64) {
        if (!first_register_read_) {
          // return value for zx_channel_read
          Populate64BitRegister(debug_ipc::RegisterID::kX64_rax,
                                zx_channel_read_result_, &registers);
        }
        first_register_read_ = false;
        // zx_handle_t handle
        Populate32BitRegister(debug_ipc::RegisterID::kX64_rdi, 0xcefa1db0,
                              &registers);
        // uint32_t options
        Populate32BitRegister(debug_ipc::RegisterID::kX64_rsi, 0, &registers);
        // bytes_address
        Populate64BitRegister(debug_ipc::RegisterID::kX64_rdx, kBytesAddress,
                              &registers);
        // handles_address
        Populate64BitRegister(debug_ipc::RegisterID::kX64_rcx, kHandlesAddress,
                              &registers);
        // num_bytes
        Populate32BitRegister(debug_ipc::RegisterID::kX64_r8, num_bytes(),
                              &registers);
        // num_handles
        Populate32BitRegister(debug_ipc::RegisterID::kX64_r9, num_handles(),
                              &registers);
        // stack pointer
        Populate64BitRegister(debug_ipc::RegisterID::kX64_rsp,
                              current_stack_ptr_, &registers);
      }
    } else {
      FXL_NOTREACHED() << "do not know what the registers should be at this IP";
    }
  }

  void Step() {
    // Increment the stack pointer to make it look as if we've stepped out of
    // the zx_channel function.
    current_stack_ptr_ += 16;
  }

  static constexpr uint64_t kWriteElfSymbolAddress = 0x100060;
  static constexpr uint64_t kReadElfSymbolAddress = 0x1000b0;
  static constexpr uint64_t kStackPointer = 0x57accadde5500;

 private:
  static const zx_txid_t kTxId = 0xaaaaaaaa;
  static const uint32_t kReserved = 0x0;
  static const uint32_t kFlags = 0x0;
  static const uint32_t kOrdinal = 2011483371;
  static constexpr char kElfSymbolBuildID[] = "123412341234";
  static constexpr uint64_t kBytesAddress = 0x7e57ab1eba5eba11;
  static constexpr uint64_t kHandlesAddress = 0xca11ab1e7e57;
  static constexpr uint64_t kActualBytesPtr = 0x2000;
  static constexpr uint64_t kActualHandlesPtr = 0x3000;
  static const char* zx_channel_write_name_;
  static const char* zx_channel_read_name_;

  uint64_t current_stack_ptr_;
  uint64_t current_symbol_address_;
  fidl_message_header_t header_;
  zx_handle_t handles_[2] = {0x01234567, 0x89abcdef};
  uint8_t stack_[3 * sizeof(uint64_t)];
  debug_ipc::Arch arch_;
  bool read_has_bytes_ = true;
  bool read_has_handles_ = true;
  bool first_register_read_ = true;
  int64_t zx_channel_read_result_ = ZX_OK;
};

const char* DataForZxChannelTest::zx_channel_write_name_ =
    InterceptionWorkflow::kZxChannelWriteName;
const char* DataForZxChannelTest::zx_channel_read_name_ =
    InterceptionWorkflow::kZxChannelReadName;

struct BreakpointCmp {
  bool operator()(const debug_ipc::BreakpointSettings& lhs,
                  const debug_ipc::BreakpointSettings& rhs) const {
    return lhs.id < rhs.id;
  }
};

// Provides the infrastructure needed to provide the data above.
class InterceptionRemoteAPI : public zxdb::MockRemoteAPI {
 public:
  explicit InterceptionRemoteAPI(DataForZxChannelTest& data) : data_(data) {}

  void AddOrChangeBreakpoint(
      const debug_ipc::AddOrChangeBreakpointRequest& request,
      std::function<void(const zxdb::Err&,
                         debug_ipc::AddOrChangeBreakpointReply)>
          cb) override {
    breakpoints_.insert(request.breakpoint);
    MockRemoteAPI::AddOrChangeBreakpoint(request, cb);
  }

  void Attach(const debug_ipc::AttachRequest& request,
              std::function<void(const zxdb::Err&, debug_ipc::AttachReply)> cb)
      override {
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb]() { cb(zxdb::Err(), debug_ipc::AttachReply()); });
  }

  void Modules(const debug_ipc::ModulesRequest& request,
               std::function<void(const zxdb::Err&, debug_ipc::ModulesReply)>
                   cb) override {
    debug_ipc::ModulesReply reply;
    data_.PopulateModules(reply.modules);
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb, reply]() { cb(zxdb::Err(), reply); });
  }

  void ReadMemory(
      const debug_ipc::ReadMemoryRequest& request,
      std::function<void(const zxdb::Err&, debug_ipc::ReadMemoryReply)> cb)
      override {
    debug_ipc::ReadMemoryReply reply;
    data_.PopulateMemoryBlockForAddress(request.address, request.size,
                                        reply.blocks.emplace_back());
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb, reply]() { cb(zxdb::Err(), reply); });
  }

  void ReadRegisters(
      const debug_ipc::ReadRegistersRequest& request,
      std::function<void(const zxdb::Err&, debug_ipc::ReadRegistersReply)> cb)
      override {
    // TODO: Parameterize this so we can have more than one test.
    debug_ipc::ReadRegistersReply reply;
    data_.PopulateRegisters(reply.categories.emplace_back());
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb, reply]() { cb(zxdb::Err(), reply); });
  }

  void Resume(const debug_ipc::ResumeRequest& request,
              std::function<void(const zxdb::Err&, debug_ipc::ResumeReply)> cb)
      override {
    debug_ipc::ResumeReply reply;
    if (data_.current_stack_ptr() == DataForZxChannelTest::kStackPointer) {
      // This means we need to step out of the zx_channel_read function.
      data_.Step();
    }
    debug_ipc::MessageLoop::Current()->PostTask(FROM_HERE, [cb, reply]() {
      cb(zxdb::Err(), reply);
      // This is so that the test can inject the next exception.
      debug_ipc::MessageLoop::Current()->QuitNow();
    });
  }

  const std::set<debug_ipc::BreakpointSettings, BreakpointCmp>& breakpoints()
      const {
    return breakpoints_;
  }

  void PopulateBreakpointIds(uint64_t address,
                             debug_ipc::NotifyException& notification) {
    for (auto& breakpoint : breakpoints_) {
      if (address == breakpoint.locations[0].address) {
        notification.hit_breakpoints.emplace_back();
        notification.hit_breakpoints.back().id = breakpoint.id;
        data_.SetCurrentAddress(address);
      }
    }
  }

 private:
  std::set<debug_ipc::BreakpointSettings, BreakpointCmp> breakpoints_;
  DataForZxChannelTest& data_;
};

class InterceptionWorkflowTest : public zxdb::RemoteAPITest {
 public:
  explicit InterceptionWorkflowTest(debug_ipc::Arch arch) : data_(arch) {}
  ~InterceptionWorkflowTest() override = default;

  InterceptionRemoteAPI& mock_remote_api() { return *mock_remote_api_; }

  std::unique_ptr<zxdb::RemoteAPI> GetRemoteAPIImpl() override {
    auto remote_api = std::make_unique<InterceptionRemoteAPI>(data_);
    mock_remote_api_ = remote_api.get();
    return std::move(remote_api);
  }

  DataForZxChannelTest& data() { return data_; }

  void WriteTest();

  void ReadTest();

 protected:
  DataForZxChannelTest data_;

 private:
  InterceptionRemoteAPI* mock_remote_api_;  // Owned by the session.
};

class InterceptionWorkflowTestX64 : public InterceptionWorkflowTest {
 public:
  InterceptionWorkflowTestX64() : InterceptionWorkflowTest(GetArch()) {}
  ~InterceptionWorkflowTestX64() override = default;

  virtual debug_ipc::Arch GetArch() const override {
    return debug_ipc::Arch::kX64;
  }
};

class InterceptionWorkflowTestArm : public InterceptionWorkflowTest {
 public:
  InterceptionWorkflowTestArm() : InterceptionWorkflowTest(GetArch()) {}
  ~InterceptionWorkflowTestArm() override = default;

  virtual debug_ipc::Arch GetArch() const override {
    return debug_ipc::Arch::kArm64;
  }
};

// This does process setup for the test.  It creates a fake process, injects
// modules with the appropriate symbols, attaches to the process, etc.
class ProcessController {
 public:
  ProcessController(InterceptionWorkflowTest* remote_api,
                    zxdb::Session& session,
                    debug_ipc::PlatformMessageLoop& loop);
  ~ProcessController();

  void Detach();

  InterceptionWorkflow& workflow() { return workflow_; }

  static constexpr uint64_t kProcessKoid = 1234;
  static constexpr uint64_t kThreadKoid = 5678;

 private:
  InterceptionWorkflow workflow_;

  zxdb::Process* process_;
  zxdb::Target* target_;
};

namespace {

template <typename T>
void AppendElements(std::string& result, size_t num, const T* a, const T* b) {
  std::ostringstream os;
  os << "actual      expected\n";
  for (size_t i = 0; i < num; i++) {
    os << std::left << std::setw(11) << a[i];
    os << " ";
    os << std::left << std::setw(11) << b[i];
    os << std::endl;
  }
  result.append(os.str());
}

class AlwaysQuit {
 public:
  AlwaysQuit(ProcessController* controller) : controller_(controller) {}
  ~AlwaysQuit() { controller_->Detach(); }

 private:
  ProcessController* controller_;
};

}  // namespace

ProcessController::ProcessController(InterceptionWorkflowTest* remote_api,
                                     zxdb::Session& session,
                                     debug_ipc::PlatformMessageLoop& loop)
    : workflow_(&session, &loop) {
  zxdb::Err err;
  std::vector<std::string> blank;
  workflow_.Initialize(blank);

  // Create a fake process and thread.
  process_ = remote_api->InjectProcess(kProcessKoid);
  zxdb::Thread* the_thread =
      remote_api->InjectThread(kProcessKoid, kThreadKoid);

  // Observe thread.  This is usually done in workflow_::Attach, but
  // RemoteAPITest has its own ideas about attaching, so that method only
  // half-works (the half that registers the target with the workflow). We have
  // to register the observer manually.
  target_ = session.system().GetTargets()[0];
  workflow_.AddObserver(target_);
  workflow_.observer_.DidCreateProcess(target_, process_, false);
  workflow_.observer_.process_observer().DidCreateThread(process_, the_thread);

  // Attach to process.
  debug_ipc::MessageLoop::Current()->PostTask(FROM_HERE, [this]() {
    workflow_.Attach(kProcessKoid, [](const zxdb::Err& err) {
      // Because we are already attached, we don't get here.
      FAIL() << "Should not be reached";
    });
    debug_ipc::MessageLoop::Current()->QuitNow();
  });
  debug_ipc::MessageLoop::Current()->Run();

  // Load modules into program (including the one with the zx_channel_write
  // and zx_channel_read symbols)
  fxl::RefPtr<zxdb::SystemSymbols::ModuleRef> module_ref =
      remote_api->data().GetModuleRef(&session);

  for (zxdb::Target* target : session.system().GetTargets()) {
    zxdb::Err err;
    std::vector<debug_ipc::Module> modules;
    // Force system to load modules.  Callback doesn't need to do anything
    // interesting.
    target->GetProcess()->GetModules(
        [](const zxdb::Err&, std::vector<debug_ipc::Module>) {
          debug_ipc::MessageLoop::Current()->QuitNow();
        });
    debug_ipc::MessageLoop::Current()->Run();
  }
}

ProcessController::~ProcessController() {
  process_->RemoveObserver(&workflow_.observer_.process_observer());
  target_->RemoveObserver(&workflow_.observer_);
}

void ProcessController::Detach() { workflow_.Detach(); }

void InterceptionWorkflowTest::WriteTest() {
  ProcessController controller(this, session(), loop());
  bool hit_breakpoint = false;
  // This will be executed when the zx_channel_write breakpoint is triggered.
  controller.workflow().SetZxChannelWriteCallback(
      [this, &controller, &hit_breakpoint](const zxdb::Err& err,
                                           const ZxChannelParams& params) {
        AlwaysQuit aq(&controller);
        hit_breakpoint = true;
        ASSERT_EQ(zxdb::ErrType::kNone, err.type()) << err.msg();

        std::string result;

        const uint8_t* data = data_.data();
        uint32_t num_bytes = params.GetNumBytes();
        ASSERT_EQ(num_bytes, data_.num_bytes());
        if (memcmp(params.GetBytes().get(), data, num_bytes) != 0) {
          result.append("bytes not equivalent");
          AppendElements<uint8_t>(result, num_bytes, params.GetBytes().get(),
                                  data);
          FAIL() << result;
        }

        const zx_handle_t* handles = data_.handles();
        uint32_t num_handles = params.GetNumHandles();
        ASSERT_EQ(num_handles, data_.num_handles());
        if (memcmp(params.GetHandles().get(), handles,
                   num_handles * sizeof(zx_handle_t)) != 0) {
          result.append("handles not equivalent\n");
          AppendElements<zx_handle_t>(result, num_handles,
                                      params.GetHandles().get(), handles);
          FAIL() << result;
        }
      });

  // Trigger breakpoint.
  debug_ipc::NotifyException notification;
  notification.type = debug_ipc::NotifyException::Type::kGeneral;
  notification.thread.process_koid = ProcessController::kProcessKoid;
  notification.thread.thread_koid = ProcessController::kThreadKoid;
  notification.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
  mock_remote_api().PopulateBreakpointIds(
      DataForZxChannelTest::kWriteElfSymbolAddress, notification);
  InjectException(notification);

  debug_ipc::MessageLoop::Current()->Run();

  // At this point, the ZxChannelWrite callback should have been executed.
  ASSERT_TRUE(hit_breakpoint);

  // Making sure shutdown works.
  debug_ipc::MessageLoop::Current()->Run();
}

TEST_F(InterceptionWorkflowTestX64, ZxChannelWrite) { WriteTest(); }

TEST_F(InterceptionWorkflowTestArm, ZxChannelWrite) { WriteTest(); }

void InterceptionWorkflowTest::ReadTest() {
  ProcessController controller(this, session(), loop());
  bool hit_breakpoint = false;
  // This will be executed when the zx_channel_read breakpoint is triggered.
  controller.workflow().SetZxChannelReadCallback(
      [this, &controller, &hit_breakpoint](const zxdb::Err& err,
                                           const ZxChannelParams& params) {
        if (data().zx_channel_read_result() != ZX_OK) {
          ASSERT_EQ(zxdb::ErrType::kGeneral, err.type()) << "error expected";
          return;
        }
        AlwaysQuit aq(&controller);
        hit_breakpoint = true;
        ASSERT_EQ(zxdb::ErrType::kNone, err.type()) << err.msg();

        std::string result;

        if (data_.read_has_bytes()) {
          const uint8_t* data = data_.data();
          uint32_t num_bytes = params.GetNumBytes();
          ASSERT_EQ(num_bytes, data_.num_bytes());
          if (memcmp(params.GetBytes().get(), data, num_bytes) != 0) {
            result.append("bytes not equivalent");
            AppendElements<uint8_t>(result, num_bytes, params.GetBytes().get(),
                                    data);
            FAIL() << result;
          }
        }

        if (data_.read_has_handles()) {
          const zx_handle_t* handles = data_.handles();
          uint32_t num_handles = params.GetNumHandles();
          ASSERT_EQ(num_handles, data_.num_handles());
          if (memcmp(params.GetHandles().get(), handles,
                     num_handles * sizeof(zx_handle_t)) != 0) {
            result.append("handles not equivalent\n");
            AppendElements<zx_handle_t>(result, num_handles,
                                        params.GetHandles().get(), handles);
            FAIL() << result;
          }
        }
      });

  {
    // Trigger initial breakpoint, on zx_channel_read
    debug_ipc::NotifyException notification;
    notification.type = debug_ipc::NotifyException::Type::kGeneral;
    notification.thread.process_koid = ProcessController::kProcessKoid;
    notification.thread.thread_koid = ProcessController::kThreadKoid;
    notification.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
    notification.thread.stack_amount =
        debug_ipc::ThreadRecord::StackAmount::kMinimal;
    debug_ipc::StackFrame frame(1, 3);
    notification.thread.frames.push_back(frame);
    mock_remote_api().PopulateBreakpointIds(
        DataForZxChannelTest::kReadElfSymbolAddress, notification);
    InjectException(notification);
  }

  debug_ipc::MessageLoop::Current()->Run();

  {
    // Trigger next breakpoint, when zx_channel_read has completed.
    debug_ipc::NotifyException notification;
    notification.type = debug_ipc::NotifyException::Type::kGeneral;
    notification.thread.process_koid = ProcessController::kProcessKoid;
    notification.thread.thread_koid = ProcessController::kThreadKoid;
    notification.thread.state = debug_ipc::ThreadRecord::State::kBlocked;
    InjectException(notification);
  }

  debug_ipc::MessageLoop::Current()->Run();

  if (data().zx_channel_read_result() == ZX_OK) {
    // At this point, the ZxChannelRead callback should have been executed.
    ASSERT_TRUE(hit_breakpoint);

    // Making sure shutdown works.
    debug_ipc::MessageLoop::Current()->Run();
  }
}

#define TEST_WITH_ARCH(arch)                                            \
  TEST_F(InterceptionWorkflowTest##arch, ZxChannelRead) { ReadTest(); } \
                                                                        \
  TEST_F(InterceptionWorkflowTest##arch, ZxChannelReadFailed) {         \
    data().set_zx_channel_read_result(ZX_ERR_SHOULD_WAIT);              \
    ReadTest();                                                         \
  }                                                                     \
                                                                        \
  TEST_F(InterceptionWorkflowTest##arch, ZxChannelReadNoHandles) {      \
    data().set_read_has_handles(false);                                 \
    ReadTest();                                                         \
  }                                                                     \
                                                                        \
  TEST_F(InterceptionWorkflowTest##arch, ZxChannelReadNoBytes) {        \
    data().set_read_has_bytes(false);                                   \
    ReadTest();                                                         \
  }

TEST_WITH_ARCH(Arm)
TEST_WITH_ARCH(X64)

}  // namespace fidlcat
