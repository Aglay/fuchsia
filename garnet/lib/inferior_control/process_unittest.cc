// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/cpp/task.h>
#include <lib/zx/channel.h>
#include <string.h>

#include "garnet/lib/inferior_control/process.h"
#include "garnet/lib/inferior_control/test_server.h"

#include "gtest/gtest.h"

namespace inferior_control {
namespace {

// TODO(dje): Obtain path more cleanly.
const char helper_program[] =
    "/pkgfs/packages/inferior_control_tests/0/bin/"
    "inferior_control_test_helper";

using ProcessTest = TestServer;

TEST_F(ProcessTest, Launch) {
  std::vector<std::string> argv{
      helper_program,
  };
  ASSERT_TRUE(SetupInferior(argv));

  EXPECT_TRUE(RunHelperProgram(zx::channel()));
  EXPECT_TRUE(Run());
  EXPECT_TRUE(TestSuccessfulExit());
}

// Test detaching and re-attaching.
// To add some determinism, we wait for the main thread to finish starting
// before detaching. This ensures we always have processed the main
// thread's ZX_EXCP_THREAD_STARTING exception before detaching.
// Note: Exceptions are handled in the same thread as server.Run().

class AttachTest : public TestServer {
 public:
  AttachTest() = default;

  void OnThreadStarting(Process* process, Thread* thread,
                        const zx_exception_context_t& context) override {
    if (!main_thread_started_) {
      // Must be the inferior's main thread.
      main_thread_started_ = true;
      async::PostTask(message_loop().dispatcher(),
                      [this] { DoDetachAttach(); });
    }
    TestServer::OnThreadStarting(process, thread, context);
  }

  void DoDetachAttach() {
    auto inferior = current_process();
    auto pid = inferior->id();
    EXPECT_TRUE(inferior->Detach());
    EXPECT_TRUE(inferior->Attach(pid));
    // If attaching failed we'll hang since we won't see the inferior exiting.
    if (!inferior->IsAttached()) {
      QuitMessageLoop(true);
    }
    // The inferior is waiting for us to close our side of the channel.
    channel_.reset();
  }

  void set_channel(zx::channel channel) { channel_ = std::move(channel); }

 private:
  bool main_thread_started_ = false;
  zx::channel channel_;
};

TEST_F(AttachTest, Attach) {
  std::vector<std::string> argv{
      helper_program,
      "wait-peer-closed",
  };
  ASSERT_TRUE(SetupInferior(argv));

  zx::channel our_channel, their_channel;
  auto status = zx::channel::create(0, &our_channel, &their_channel);
  ASSERT_EQ(status, ZX_OK);

  EXPECT_TRUE(RunHelperProgram(std::move(their_channel)));
  set_channel(std::move(our_channel));

  EXPECT_TRUE(Run());
  EXPECT_TRUE(TestSuccessfulExit());
}

class FindThreadByIdTest : public TestServer {
 public:
  FindThreadByIdTest() = default;

  void OnThreadStarting(Process* process, Thread* thread,
                        const zx_exception_context_t& context) override {
    thread_koid_ = thread->id();
    Thread* lookup_thread = process->FindThreadById(thread_koid_);
    if (lookup_thread) {
      found_thread_by_id_ = true;
    }
    TestServer::OnThreadStarting(process, thread, context);
  }

  zx_koid_t thread_koid() const { return thread_koid_; }
  bool found_thread_by_id() const { return found_thread_by_id_; }

 private:
  bool found_thread_by_id_ = false;
  zx_koid_t thread_koid_ = ZX_KOID_INVALID;
};

TEST_F(FindThreadByIdTest, FindThreadById) {
  std::vector<std::string> argv{
      helper_program,
  };
  ASSERT_TRUE(SetupInferior(argv));

  EXPECT_TRUE(RunHelperProgram(zx::channel{}));

  EXPECT_TRUE(Run());
  EXPECT_TRUE(TestSuccessfulExit());
  EXPECT_TRUE(found_thread_by_id());
  Process* process = current_process();
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(process->FindThreadById(thread_koid()), nullptr);
}

class LdsoBreakpointTest : public TestServer {
 public:
  LdsoBreakpointTest() = default;

  bool dsos_loaded() const { return dsos_loaded_; }
  bool libc_present() const { return libc_present_; }
  bool exec_present() const { return exec_present_; }

  void OnArchitecturalException(
      Process* process, Thread* thread, const zx_excp_type_t type,
      const zx_exception_context_t& context) {
    FXL_LOG(INFO) << "Got exception 0x" << std::hex << type;
    if (type == ZX_EXCP_SW_BREAKPOINT) {
      // The shared libraries should have been loaded by now.
      if (process->DsosLoaded()) {
        dsos_loaded_ = true;
        // Libc and the main executable should be present.
        for (debugger_utils::dsoinfo_t* dso = process->GetDsos(); dso;
             dso = dso->next) {
          FXL_VLOG(1) << "Have dso " << dso->name;
          // The main executable's name might either be recorded as "" or
          // a potentially clipped version of the path in which case
          // "inferior_control_tests" should still be present.
          if (strcmp(dso->name, "") == 0 ||
              strstr(dso->name, "inferior_control_tests") != nullptr) {
            exec_present_ = true;
          } else if (strcmp(dso->name, "libc.so") == 0) {
            libc_present_ = true;
          }
        }
      }

      // Terminate the inferior, we don't want the exception propagating to
      // the system exception handler.
      zx_task_kill(process->handle());
    }
  }

 private:
  bool dsos_loaded_ = false;
  bool libc_present_ = false;
  bool exec_present_ = false;
};

TEST_F(LdsoBreakpointTest, LdsoBreakpoint) {
  std::vector<std::string> argv{
    helper_program,
    "test-try-next",
  };
  ASSERT_TRUE(SetupInferior(argv));

  zx::channel our_channel, their_channel;
  auto status = zx::channel::create(0, &our_channel, &their_channel);
  ASSERT_EQ(status, ZX_OK);

  EXPECT_TRUE(RunHelperProgram(std::move(their_channel)));

  // The inferior is waiting for us to close our side of the channel.
  our_channel.reset();

  EXPECT_TRUE(Run());
  EXPECT_TRUE(dsos_loaded());
  EXPECT_TRUE(libc_present());
  EXPECT_TRUE(exec_present());
}

class KillTest : public TestServer {
 public:
  KillTest() = default;

  void OnThreadStarting(Process* process, Thread* thread,
                        const zx_exception_context_t& context) override {
    kill_requested_ = process->Kill();
    TestServer::OnThreadStarting(process, thread, context);
  }

  bool kill_requested() const { return kill_requested_; }

  void set_channel(zx::channel channel) { channel_ = std::move(channel); }

 private:
  bool kill_requested_ = false;
  zx::channel channel_;
};

TEST_F(KillTest, Kill) {
  std::vector<std::string> argv{
      helper_program,
      "wait-peer-closed",
  };
  ASSERT_TRUE(SetupInferior(argv));

  zx::channel our_channel, their_channel;
  auto status = zx::channel::create(0, &our_channel, &their_channel);
  ASSERT_EQ(status, ZX_OK);

  EXPECT_TRUE(RunHelperProgram(std::move(their_channel)));

  EXPECT_TRUE(Run());
  EXPECT_TRUE(TestFailureExit());
  EXPECT_TRUE(kill_requested());
}

}  // namespace
}  // namespace inferior_control
