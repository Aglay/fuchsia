// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:fxtest/fxtest.dart';

/// Concrete flag for an individual test, indicating how it should be executed.
///
/// Note that [unsupported] is included as a bucket for tests we have failed
/// to account for. It is not an acceptable place for tests to end up. Should
/// any tests find their way here, an exception will be raised that will halt
/// test execution entirely (but which can be silenced with a flag).
enum TestType {
  command,
  component,
  host,
  suite,

  /// Catch-all for a test we know we haven't correctly included handling logic
  unsupported,

  /// Non-component but on-device tests (an illegal and mostly legacy configuration)
  unsupportedDeviceTest,
}

const Set<TestType> hostTestTypes = {
  TestType.command,
  TestType.host,
};

const Set<TestType> unsupportedTestTypes = {
  TestType.unsupportedDeviceTest,
  TestType.unsupported,
};

class ExecutionHandle {
  final String fx;
  final String handle;
  final String os;
  final TestType testType;
  ExecutionHandle(this.fx, this.handle, this.os, {this.testType});
  ExecutionHandle.command(this.fx, this.handle, this.os)
      : testType = TestType.command;
  ExecutionHandle.component(this.fx, this.handle, this.os)
      : testType = TestType.component;
  ExecutionHandle.suite(this.fx, this.handle, this.os)
      : testType = TestType.suite;
  ExecutionHandle.host(this.fx, this.handle, this.os)
      : testType = TestType.host;
  ExecutionHandle.unsupportedDeviceTest(this.handle)
      : fx = '',
        os = 'fuchsia',
        testType = TestType.unsupportedDeviceTest;
  ExecutionHandle.unsupported()
      : fx = '',
        handle = '',
        os = '',
        testType = TestType.unsupported;

  bool get isUnsupported => unsupportedTestTypes.contains(testType);

  /// Produces the complete list of tokens required to invoke this test.
  ///
  /// This does not account for any extra tokens the user many require - here
  /// we are only considered with vanilla test invocations driven straight from
  /// the definition in the manifest.
  CommandTokens getInvocationTokens(List<String> runnerFlags) {
    if (testType == TestType.command) {
      return _getCommandTokens();
    } else if (testType == TestType.component) {
      return _getComponentTokens(runnerFlags);
    } else if (testType == TestType.host) {
      return _getHostTokens();
    } else if (testType == TestType.suite) {
      return _getSuiteTokens();
    }
    return CommandTokens.empty();
  }

  /// Handler for test definitions using the "command" keyword.
  ///
  /// Handles tests containing a key like so:
  /// ```json
  /// {"command": ["host_x64/some_binary", "--some-flags"]}
  /// ```
  CommandTokens _getCommandTokens() {
    List<String> commandTokens = handle.split(' ');

    // Currently, some entries in `tests.json` appear due to a bug, and as such,
    // simply with the command ["run", "..."]. We need to coerce that to its
    // correct syntax, but with a helpful warning.
    if (commandTokens.first == 'run') {
      return CommandTokens(
        [fx, 'shell', ...commandTokens.sublist(1)],
        warning:
            'Warning! Only host tests are expected to use the "command" syntax. '
            'The test [$commandTokens] did not comply with this expectation.',
      );
    }
    return CommandTokens(commandTokens);
  }

  /// Handler for `tests.json` entries containing the `packageUrl` key ending
  /// in ".cmx".
  CommandTokens _getComponentTokens(List<String> runnerFlags) {
    List<String> subCommand = ['shell', 'run-test-component'] + runnerFlags;
    return CommandTokens([fx, ...subCommand, handle]);
  }

  /// Handler for `tests.json` entries containing the `packageUrl` key ending
  /// in ".cm".
  CommandTokens _getSuiteTokens() {
    List<String> subCommand = ['shell', 'run-test-suite'];
    return CommandTokens([fx, ...subCommand, handle]);
  }

  /// Handler for `tests.json` entries containing the `path` key.
  CommandTokens _getHostTokens() {
    return CommandTokens([handle]);
  }
}
