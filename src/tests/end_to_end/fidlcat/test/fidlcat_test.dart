// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' show Platform, Process, ProcessResult;

import 'package:logging/logging.dart';
import 'package:test/test.dart';
import 'package:sl4f/sl4f.dart' as sl4f;

const _timeout = Timeout(Duration(minutes: 5));

/// Formats an IP address so that fidlcat can understand it (removes % part,
/// adds brackets around it.)
String formatTarget(Logger log, String target) {
  log.info('$target: target');
  try {
    Uri.parseIPv4Address(target);
    return target;
  } on FormatException {
    try {
      Uri.parseIPv6Address(target);
      return '[$target]';
    } on FormatException {
      try {
        Uri.parseIPv6Address(target.split('%')[0]);
        return '[$target]';
      } on FormatException {
        return null;
      }
    }
  }
}

class RunFidlcat {
  String target;
  int port;
  Future<ProcessResult> agentResult;
  String stdout;
  String stderr;
  String additionalResult;

  Future<void> run(Logger log, sl4f.Sl4f sl4fDriver, String path,
      List<String> extraArguments) async {
    port = await sl4fDriver.ssh.pickUnusedPort();
    log.info('Chose port: $port');

    /// fuchsia-pkg URL for the debug agent.
    const String debugAgentUrl =
        'fuchsia-pkg://fuchsia.com/debug_agent#meta/debug_agent.cmx';

    agentResult = sl4fDriver.ssh.run('run $debugAgentUrl --port=$port');
    target = formatTarget(log, sl4fDriver.ssh.target);
    log.info('Target: $target');

    List<String> arguments;
    final String symbolPath = Platform.script
        .resolve('runtime_deps/echo_client_cpp.debug')
        .toFilePath();
    // We have to list all of the IR we need explicitly, here and in the BUILD.gn file. The
    // two lists must be kept in sync: if you add an IR here, you must also add it to the
    // BUILD.gn file.
    final String echoIr =
        Platform.script.resolve('runtime_deps/echo.fidl.json').toFilePath();
    final String ioIr = Platform.script
        .resolve('runtime_deps/fuchsia-io.fidl.json')
        .toFilePath();
    final String sysIr = Platform.script
        .resolve('runtime_deps/fuchsia.sys.fidl.json')
        .toFilePath();
    arguments = [
      '--connect=$target:$port',
      '--quit-agent-on-exit',
      '--fidl-ir-path=$echoIr',
      '--fidl-ir-path=$ioIr',
      '--fidl-ir-path=$sysIr',
      '-s',
      '$symbolPath',
    ]
      ..addAll(extraArguments)
      ..addAll([
        'run',
        'fuchsia-pkg://fuchsia.com/echo_client_cpp#meta/echo_client_cpp.cmx',
      ]);
    ProcessResult processResult;
    do {
      processResult = await Process.run(path, arguments);
    } while (processResult.exitCode == 2); // 2 means can't connect (yet).

    stdout = processResult.stdout.toString();
    stderr = processResult.stderr.toString();
    additionalResult = 'stderr ===\n$stderr\nstdout ===\n$stdout';
  }
}

void main(List<String> arguments) {
  final log = Logger('fidlcat_test');

  /// Location of the fidlcat executable.
  final String fidlcatPath =
      Platform.script.resolve('runtime_deps/fidlcat').toFilePath();

  sl4f.Sl4f sl4fDriver;

  setUp(() async {
    sl4fDriver = sl4f.Sl4f.fromEnvironment();
    await sl4fDriver.startServer();
  });

  tearDown(() async {
    await sl4fDriver.stopServer();
    sl4fDriver.close();
  });

  /// Simple test to ensure that fidlcat can run the echo client, and that some of the expected
  /// output is present.  It starts the agent on the target, and then launches fidlcat with the
  /// correct parameters.
  group('fidlcat', () {
    test('Simple test of echo client output and shutdown', () async {
      var instance = RunFidlcat();
      await instance.run(log, sl4fDriver, fidlcatPath, []);

      expect(
          instance.stdout,
          contains(
              'sent request fidl.examples.echo/Echo.EchoString = {"value":"hello world"}'),
          reason: instance.additionalResult);

      await instance.agentResult;
    });

    test('Test --extra-name', () async {
      var instance = RunFidlcat();
      await instance.run(log, sl4fDriver, fidlcatPath,
          ['--remote-name=echo_server', '--extra-name=echo_client']);

      final lines = instance.stdout.split('\n\n');

      /// If we had use --remote-name twice, we would have a lot of messages between
      /// "Monitoring echo_client" and "Monitoring echo_server".
      /// With --extra-name for echo_client, we wait for echo_server before monitoring echo_client.
      /// Therefore, both line are one after the other.
      expect(lines[1], contains('Monitoring echo_client_cpp.cmx koid='),
          reason: instance.additionalResult);

      expect(lines[2], contains('Monitoring echo_server_cpp.cmx koid='),
          reason: instance.additionalResult);

      await instance.agentResult;
    });

    test('Test --trigger', () async {
      var instance = RunFidlcat();
      await instance
          .run(log, sl4fDriver, fidlcatPath, ['--trigger=.*EchoString']);

      final lines = instance.stdout.split('\n\n');

      /// The first displayed message must be EchoString.
      expect(
          lines[2],
          contains(
              'sent request fidl.examples.echo/Echo.EchoString = {"value":"hello world"}'),
          reason: instance.additionalResult);

      await instance.agentResult;
    });

    test('Test --messages', () async {
      var instance = RunFidlcat();
      await instance.run(log, sl4fDriver, fidlcatPath, [
        '--messages=.*EchoString',
        '--exclude-syscalls=zx_channel_create'
      ]);

      final lines = instance.stdout.split('\n\n');

      /// The first and second displayed messages must be EchoString (everything else has been
      /// filtered out).
      expect(lines[2], contains('sent request fidl.examples.echo/Echo.EchoString = {"value":"hello world"}'),
          reason: instance.additionalResult);
      expect(lines[3], contains('received response fidl.examples.echo/Echo.EchoString = {"response":"hello world"}'),
          reason: instance.additionalResult);

      await instance.agentResult;
    });
  }, timeout: _timeout);
}
