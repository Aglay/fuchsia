// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/exception_broker/exception_broker.h"

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fsl/handles/object_info.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/syslog/cpp/logger.h>
#include <zircon/status.h>
#include <zircon/syscalls/exception.h>

#include <type_traits>

#include <garnet/public/lib/fostr/fidl/fuchsia/exception/formatting.h>
#include <gtest/gtest.h>
#include <third_party/crashpad/snapshot/minidump/process_snapshot_minidump.h>
#include <third_party/crashpad/util/file/string_file.h>

#include "src/developer/exception_broker/tests/crasher_wrapper.h"
#include "src/lib/fxl/test/test_settings.h"

namespace fuchsia {
namespace exception {

void ToString(const ExceptionType& value, std::ostream* os) {
  *os << value;
}

namespace {

// ExceptionBroker unit test -----------------------------------------------------------------------
//
// This test is meant to verify that the exception broker does the correct thing depending on the
// configuration. The main objective of this test is to verify that the connected crash reporter and
// exception handlers actually receive the exception from the broker.

class StubCrashReporter : public fuchsia::feedback::CrashReporter {
 public:
  void File(fuchsia::feedback::CrashReport report, FileCallback callback) {
    reports_.push_back(std::move(report));

    fuchsia::feedback::CrashReporter_File_Result result;
    result.set_response({});
    callback(std::move(result));
  }

  fidl::InterfaceRequestHandler<fuchsia::feedback::CrashReporter> GetHandler() {
    return [this](fidl::InterfaceRequest<fuchsia::feedback::CrashReporter> request) {
      bindings_.AddBinding(this, std::move(request));
    };
  }

  const std::vector<fuchsia::feedback::CrashReport>& reports() const { return reports_; }

 private:
  std::vector<fuchsia::feedback::CrashReport> reports_;

  fidl::BindingSet<fuchsia::feedback::CrashReporter> bindings_;
};

// Test Setup --------------------------------------------------------------------------------------
//
// Necessary elements for a fidl test to run. The ServiceDirectoryProvider is meant to mock the
// environment from which a process gets its services. This is the way we "inject" in our stub
// crash reporter instead of the real one.

struct TestContext {
  async::Loop loop;
  sys::testing::ServiceDirectoryProvider services;
  std::unique_ptr<StubCrashReporter> crash_reporter;
};

std::unique_ptr<TestContext> CreateTestContext() {
  std::unique_ptr<TestContext> context(new TestContext{
      .loop = async::Loop(&kAsyncLoopConfigAttachToCurrentThread),
      .services = sys::testing::ServiceDirectoryProvider{},
      .crash_reporter = std::make_unique<StubCrashReporter>(),
  });

  return context;
}

// Runs a loop until |condition| is true. Does this by stopping every |step| to check the condition.
// If |condition| is never true, the thread will never leave this cycle.
// The test harness has to be able to handle this "hanging" case.
void RunUntil(TestContext* context, fit::function<bool()> condition,
              zx::duration step = zx::msec(10)) {
  while (!condition()) {
    context->loop.Run(zx::deadline_after(step));
  }
}

bool RetrieveExceptionContext(ExceptionContext* pe) {
  // Create a process that crashes and obtain the relevant handles and exception.
  // By the time |SpawnCrasher| has returned, the process has already thrown an exception.
  if (!SpawnCrasher(pe))
    return false;

  // We mark the exception to be handled. We need this because we pass on the exception to the
  // handler, which will resume it before we get the control back. If we don't mark it as handled,
  // the exception will bubble out of our environment.
  return MarkExceptionAsHandled(pe);
}

ExceptionInfo ExceptionContextToExceptionInfo(const ExceptionContext& pe) {
  // Translate the exception to the fidl format.
  ExceptionInfo exception_info;
  exception_info.process_koid = pe.exception_info.pid;
  exception_info.thread_koid = pe.exception_info.tid;
  exception_info.type = static_cast<ExceptionType>(pe.exception_info.type);

  return exception_info;
}

// Utilities ---------------------------------------------------------------------------------------

inline void ValidateReport(const fuchsia::feedback::CrashReport& report, bool validate_minidump) {
  ASSERT_TRUE(report.has_program_name());

  ASSERT_TRUE(report.has_specific_report());
  const fuchsia::feedback::SpecificCrashReport& specific_report = report.specific_report();

  ASSERT_TRUE(specific_report.is_native());
  const fuchsia::feedback::NativeCrashReport& native_report = specific_report.native();

  // If the broker could not get a minidump, it will not send a mem buffer.
  if (!validate_minidump) {
    ASSERT_FALSE(native_report.has_minidump());
    return;
  }

  EXPECT_EQ(report.program_name(), "crasher");

  ASSERT_TRUE(native_report.has_minidump());
  const zx::vmo& minidump_vmo = native_report.minidump().vmo;

  uint64_t vmo_size;
  ASSERT_EQ(minidump_vmo.get_size(&vmo_size), ZX_OK);

  auto buf = std::make_unique<uint8_t[]>(vmo_size);
  ASSERT_EQ(minidump_vmo.read(buf.get(), 0, vmo_size), ZX_OK);

  // Read the vmo back into a file writer/reader interface.
  crashpad::StringFile string_file;
  string_file.Write(buf.get(), vmo_size);

  // Move the cursor to the beggining of the file.
  ASSERT_EQ(string_file.Seek(0, SEEK_SET), 0);

  // We verify that the minidump snapshot can validly read the file.
  crashpad::ProcessSnapshotMinidump minidump_snapshot;
  ASSERT_TRUE(minidump_snapshot.Initialize(&string_file));
}

bool ValidateException(const ProcessExceptionMetadata&) { return true; }
bool ValidateException(const ProcessException& exception) { return exception.has_exception(); }

template <typename T>
void ValidateException(const ExceptionContext& context, const T& process_exception) {
  if (std::is_same<T, ProcessException>::value)
    ASSERT_TRUE(ValidateException(process_exception));
  ASSERT_TRUE(process_exception.has_info());
  ASSERT_TRUE(process_exception.has_process());
  ASSERT_TRUE(process_exception.has_thread());

  const zx::process& process = process_exception.process();
  ASSERT_EQ(context.process_koid, fsl::GetKoid(process.get()));
  ASSERT_EQ(context.process_koid, process_exception.info().process_koid);
  ASSERT_EQ(context.process_name, fsl::GetObjectName(process.get()));

  const zx::thread& thread = process_exception.thread();
  ASSERT_EQ(context.thread_koid, fsl::GetKoid(thread.get()));
  ASSERT_EQ(context.thread_koid, process_exception.info().thread_koid);
  ASSERT_EQ(context.thread_name, fsl::GetObjectName(thread.get()));

  ASSERT_EQ(process_exception.info().type, ExceptionType::FATAL_PAGE_FAULT);
}

  // Tests
  // -------------------------------------------------------------------------------------------

  TEST(ExceptionBroker, CallingMultipleExceptions) {
    auto test_context = CreateTestContext();

    // We add the service we're injecting.
    test_context->services.AddService(test_context->crash_reporter->GetHandler());

    auto broker = ExceptionBroker::Create(test_context->loop.dispatcher(),
                                          test_context->services.service_directory());
    ASSERT_TRUE(broker);

    // We create multiple exceptions.
    ExceptionContext excps[3];
    ASSERT_TRUE(RetrieveExceptionContext(excps + 0));
    ASSERT_TRUE(RetrieveExceptionContext(excps + 1));
    ASSERT_TRUE(RetrieveExceptionContext(excps + 2));

    // Get the fidl representation of the exception.
    ExceptionInfo infos[3];
    infos[0] = ExceptionContextToExceptionInfo(excps[0]);
    infos[1] = ExceptionContextToExceptionInfo(excps[1]);
    infos[2] = ExceptionContextToExceptionInfo(excps[2]);

    // It's not easy to pass array references to lambdas.
    bool cb_call0 = false;
    bool cb_call1 = false;
    bool cb_call2 = false;
    broker->OnException(std::move(excps[0].exception), infos[0],
                        [&cb_call0]() { cb_call0 = true; });
    broker->OnException(std::move(excps[1].exception), infos[1],
                        [&cb_call1]() { cb_call1 = true; });
    broker->OnException(std::move(excps[2].exception), infos[2],
                        [&cb_call2]() { cb_call2 = true; });

    // There should be many connections opened.
    ASSERT_EQ(broker->connections().size(), 3u);

    // We wait until the crash reporter has received all exceptions.
    RunUntil(test_context.get(),
             [&test_context]() { return test_context->crash_reporter->reports().size() == 3u; });

    EXPECT_TRUE(cb_call0);
    EXPECT_TRUE(cb_call1);
    EXPECT_TRUE(cb_call2);

    // All connections should be killed now.
    EXPECT_EQ(broker->connections().size(), 0u);

    auto& reports = test_context->crash_reporter->reports();
    ValidateReport(reports[0], true);
    ValidateReport(reports[1], true);
    ValidateReport(reports[2], true);

    // We kill the jobs. This kills the underlying process. We do this so that the crashed process
    // doesn't get rescheduled. Otherwise the exception on the crash program would bubble out of our
    // environment and create noise on the overall system.
    excps[0].job.kill();
    excps[1].job.kill();
    excps[2].job.kill();

    // Process limbo should be empty.
    bool called = false;
    std::vector<ProcessExceptionMetadata> exceptions;
    broker->ListProcessesWaitingOnException(
        [&called, &exceptions](std::vector<ProcessExceptionMetadata> limbo) {
          called = true;
          exceptions = std::move(limbo);
        });

    ASSERT_TRUE(called);
    ASSERT_TRUE(exceptions.empty());
  }

  TEST(ExceptionBroker, NoConnection) {
    // We don't inject a stub service. This will make connecting to the service fail.
    auto test_context = CreateTestContext();

    auto broker = ExceptionBroker::Create(test_context->loop.dispatcher(),
                                          test_context->services.service_directory());
    ASSERT_TRUE(broker);

    // Create the exception.
    ExceptionContext exception;
    ASSERT_TRUE(RetrieveExceptionContext(&exception));
    ExceptionInfo info = ExceptionContextToExceptionInfo(exception);

    bool called = false;
    broker->OnException(std::move(exception.exception), info, [&called]() { called = true; });

    // There should be an outgoing connection.
    ASSERT_EQ(broker->connections().size(), 1u);

    RunUntil(test_context.get(), [&broker]() { return broker->connections().empty(); });
    ASSERT_TRUE(called);

    // The stub shouldn't be called.
    ASSERT_EQ(test_context->crash_reporter->reports().size(), 0u);

    // We kill the jobs. This kills the underlying process. We do this so that the crashed process
    // doesn't get rescheduled. Otherwise the exception on the crash program would bubble out of our
    // environment and create noise on the overall system.
    exception.job.kill();

    // Process limbo should be empty.
    called = false;
    std::vector<ProcessExceptionMetadata> exceptions;
    broker->ListProcessesWaitingOnException(
        [&called, &exceptions](std::vector<ProcessExceptionMetadata> limbo) {
          called = true;
          exceptions = std::move(limbo);
        });

    ASSERT_TRUE(called);
    ASSERT_TRUE(exceptions.empty());
  }

  TEST(ExceptionBroker, GettingInvalidVMO) {
    auto test_context = CreateTestContext();
    test_context->services.AddService(test_context->crash_reporter->GetHandler());

    auto broker = ExceptionBroker::Create(test_context->loop.dispatcher(),
                                          test_context->services.service_directory());
    ASSERT_TRUE(broker);

    // We create a bogus exception, which will fail to create a valid VMO.
    bool called = false;
    ExceptionInfo info = {};
    broker->OnException({}, info, [&called]() { called = true; });

    ASSERT_EQ(broker->connections().size(), 1u);
    RunUntil(test_context.get(), [&broker]() { return broker->connections().empty(); });
    ASSERT_TRUE(called);

    ASSERT_EQ(test_context->crash_reporter->reports().size(), 1u);
    auto& report = test_context->crash_reporter->reports().front();

    ValidateReport(report, false);
  }

  TEST(ExceptionBroker, ProcessLimbo) {
    auto test_context = CreateTestContext();
    test_context->services.AddService(test_context->crash_reporter->GetHandler());

    auto broker = ExceptionBroker::Create(test_context->loop.dispatcher(),
                                          test_context->services.service_directory());
    ASSERT_TRUE(broker);
    broker->set_use_limbo(true);

    // We create multiple exceptions.
    ExceptionContext excps[3];
    ASSERT_TRUE(RetrieveExceptionContext(excps + 0));
    ASSERT_TRUE(RetrieveExceptionContext(excps + 1));
    ASSERT_TRUE(RetrieveExceptionContext(excps + 2));

    // Get the fidl representation of the exception.
    ExceptionInfo infos[3];
    infos[0] = ExceptionContextToExceptionInfo(excps[0]);
    infos[1] = ExceptionContextToExceptionInfo(excps[1]);
    infos[2] = ExceptionContextToExceptionInfo(excps[2]);

    // It's not easy to pass array references to lambdas.
    bool cb_call0 = false;
    bool cb_call1 = false;
    bool cb_call2 = false;
    broker->OnException(std::move(excps[0].exception), infos[0],
                        [&cb_call0]() { cb_call0 = true; });
    broker->OnException(std::move(excps[1].exception), infos[1],
                        [&cb_call1]() { cb_call1 = true; });
    broker->OnException(std::move(excps[2].exception), infos[2],
                        [&cb_call2]() { cb_call2 = true; });

    // There should not be an outgoing connection and no reports generated.
    ASSERT_TRUE(broker->connections().empty());
    ASSERT_TRUE(test_context->crash_reporter->reports().empty());

    // Process limbo should have the exceptions.
    ASSERT_EQ(broker->limbo().size(), 3u);
    bool called = false;
    std::vector<ProcessExceptionMetadata> exceptions;
    broker->ListProcessesWaitingOnException(
        [&called, &exceptions](std::vector<ProcessExceptionMetadata> limbo) {
          called = true;
          exceptions = std::move(limbo);
        });

    ASSERT_TRUE(called);
    ASSERT_EQ(exceptions.size(), 3u);
    ValidateException(excps[0], exceptions[0]);
    ValidateException(excps[1], exceptions[1]);
    ValidateException(excps[2], exceptions[2]);

    // Getting a exception for a process that doesn't exist should fail.
    called = false;
    ProcessLimbo_RetrieveException_Result result;
    broker->RetrieveException(-1, [&called, &result](ProcessLimbo_RetrieveException_Result res) {
      called = true;
      result = std::move(res);
    });
    ASSERT_TRUE(called);
    ASSERT_TRUE(result.is_err());

    // There should still be 3 exceptions.
    ASSERT_EQ(broker->limbo().size(), 3u);

    // Getting an actual exception should work.
    called = false;
    result = {};
    broker->RetrieveException(infos[0].process_koid,
                              [&called, &result](ProcessLimbo_RetrieveException_Result res) {
                                called = true;
                                result = std::move(res);
                              });
    ASSERT_TRUE(called);
    ASSERT_TRUE(result.is_response());
    ValidateException(excps[0], result.response().process_exception);

    // There should be one less exception.
    ASSERT_EQ(broker->limbo().size(), 2u);

    // That process shoudl've been removed.
    called = false;
    result = {};
    broker->RetrieveException(infos[0].process_koid,
                              [&called, &result](ProcessLimbo_RetrieveException_Result res) {
                                called = true;
                                result = std::move(res);
                              });
    ASSERT_TRUE(called);
    ASSERT_TRUE(result.is_err());

    // Asking for the other process should work.
    called = false;
    result = {};
    broker->RetrieveException(infos[2].process_koid,
                              [&called, &result](ProcessLimbo_RetrieveException_Result res) {
                                called = true;
                                result = std::move(res);
                              });
    ASSERT_TRUE(called);
    ASSERT_TRUE(result.is_response());
    ValidateException(excps[2], result.response().process_exception);

    // There should be one less exception.
    ASSERT_EQ(broker->limbo().size(), 1u);

    // Getting the last one should work.
    called = false;
    result = {};
    broker->RetrieveException(infos[1].process_koid,
                              [&called, &result](ProcessLimbo_RetrieveException_Result res) {
                                called = true;
                                result = std::move(res);
                              });
    ASSERT_TRUE(called);
    ASSERT_TRUE(result.is_response());
    ValidateException(excps[1], result.response().process_exception);

    // There should be one less exception.
    ASSERT_EQ(broker->limbo().size(), 0u);

    // We kill the jobs. This kills the underlying process. We do this so that the crashed process
    // doesn't get rescheduled. Otherwise the exception on the crash program would bubble out of our
    // environment and create noise on the overall system.
    excps[0].job.kill();
    excps[1].job.kill();
    excps[2].job.kill();
  }

}  // namespace
}  // namespace exception
}  // namespace fuchsia

int main(int argc, char* argv[]) {
  if (!fxl::SetTestSettings(argc, argv))
    return EXIT_FAILURE;

  testing::InitGoogleTest(&argc, argv);
  syslog::InitLogger({"exception-broker", "unittest"});

  return RUN_ALL_TESTS();
}
