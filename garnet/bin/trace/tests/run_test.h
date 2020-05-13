// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_TRACE_TESTS_RUN_TEST_H_
#define GARNET_BIN_TRACE_TESTS_RUN_TEST_H_

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>

#include <string>
#include <vector>

namespace tracing {
namespace test {

// Our component's tmp directory.
constexpr char kTestTmpPath[] = "/tmp";

// Path to use for package-relative paths.
constexpr char kTestPackagePath[] = "/pkg";

// Path to our package for use in spawned processes.
// Our component's /pkg directory is bound to this path in the spawned process.
// This is useful when wanting the trace program to be able to read our tspec files.
constexpr char kSpawnedTestPackagePath[] = "/test-pkg";

// Path to our tmp directory for use in spawned processes.
// Our component's /tmp directory is bound to this path in the spawned process.
// This is useful when wanting the trace program to write output to our /tmp directory.
constexpr char kSpawnedTestTmpPath[] = "/test-tmp";

// Run the trace program, but do not wait for it to exit.
// |args| is the list of arguments to pass. It is not called |argv| as it does not include argv[0].
// Wait for trace to exit with |WaitAndGetReturnCode()|.
// The only current reason to use this instead of |RunTraceAndWait()| is when one is expecting a
// non-zero return code from trace.
bool RunTrace(const zx::job& job, const std::vector<std::string>& args, zx::process* out_child);

// Run the trace program and wait for it to exit.
// Returns true if trace ran successfully and exited with a zero return code.
// |args| is the list of arguments to pass. It is not called |argv| as it does not include argv[0].
bool RunTraceAndWait(const zx::job& job, const std::vector<std::string>& args);

// We don't need to pass a context to RunTspec because the trace program
// is currently a system app. If that changes then we will need a context
// to run the trace too.
bool RunTspec(const std::string& relative_tspec_path, const std::string& relative_output_file_path,
              const syslog::LogSettings& log_settings);

// N.B. This is a synchronous call that uses an internal async loop.
// ("synchronous" meaning that it waits for the verifier to complete).
bool VerifyTspec(const std::string& relative_tspec_path,
                 const std::string& relative_output_file_path,
                 const syslog::LogSettings& log_settings);

}  // namespace test
}  // namespace tracing

#endif  // GARNET_BIN_TRACE_TESTS_RUN_TEST_H_
