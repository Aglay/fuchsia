// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';

import 'package:pedantic/pedantic.dart';
import 'package:logging/logging.dart';

import 'ssh.dart';

final _log = Logger('inspect');

class Inspect {
  final Ssh ssh;

  /// Construct an [Inspect] object.
  Inspect(this.ssh);

  /// Obtains the root inspect object for a component whose path includes
  /// [componentName].
  ///
  /// This is equivalent to calling retrieveHubEntries and inspectRecursively in
  /// series.
  /// Returns null when there are no entries matching.
  Future<dynamic> inspectComponentRoot(Pattern componentName) async {
    final entries = await retrieveHubEntries(filter: componentName);
    if (entries.isEmpty) {
      return null;
    }

    final jsonResult = await inspectRecursively(entries);
    if (jsonResult == null) {
      return null;
    }

    // Workaround for bug:36468
    // TODO(crjohns): Remove after fix rolls.
    if (jsonResult.single['contents']['root'].containsKey('root')) {
      return jsonResult.single['contents']['root']['root'];
    }

    return jsonResult.single['contents']['root'];
  }

  /// Retrieves the inpect node(s) of [hubEntries], recursively, as a json
  /// object.
  ///
  /// Returns null if there's no inspect information matching for those entries.
  /// Otherwise a parsed JSON as formated by
  /// //src/lib/inspect_deprecated/query/json_formatter.cc is
  /// returned.
  Future<dynamic> inspectRecursively(List<String> entries) async {
    final hubEntries = entries.join(' ');
    final stringInspectResult = await _stdOutForSshCommand(
        'iquery --format=json --recursive $hubEntries');

    if (stringInspectResult == null) {
      return null;
    }

    return json.decode(stringInspectResult);
  }

  /// Retrieves a list of hub entries.
  ///
  /// If [filter] is set, only those entries containing [filter] are returned.
  /// If there are no matches, an empty list is returned.
  Future<List<String>> retrieveHubEntries({Pattern filter}) async {
    final stringFindResult = await _stdOutForSshCommand('iquery --find /hub');
    if (stringFindResult == null) {
      return [];
    }

    return stringFindResult
        .split('\n')
        .where((line) => filter == null || line.contains(filter))
        .toList();
  }

  /// Runs [command] in an ssh process to completion.
  ///
  /// Returns stdout of that process or null if the process exited with non-zero
  /// exit code.
  /// Upon failure, the command will be retried [retries] times. With an
  /// exponential backoff delay in between.
  Future<String> _stdOutForSshCommand(String command,
      {int retries = 3,
      Duration initialDelay = const Duration(seconds: 1)}) async {
    Future<String> attempt() async {
      final process = await ssh.run(command);
      return process.exitCode != 0 ? null : process.stdout;
    }

    final result = await attempt();
    if (result != null) {
      return result;
    }
    var delay = initialDelay;
    for (var i = 0; i < retries; i++) {
      _log.info('Command failed on attempt ${i + 1} of ${retries + 1}.'
          '  Waiting ${delay.inMilliseconds}ms before trying again...');
      await Future.delayed(delay);
      final result = await attempt();
      if (result != null) {
        return result;
      }
      delay *= 2;
    }
    _log.info('Command failed on attempt ${retries + 1} of ${retries + 1}.'
        '  No more retries remain.');
    return null;
  }
}

/// Attempts to detect freezes related to:
/// TODO(b/139742629): Device freezes for a minute after startup
/// TODO(fxb/35898): FTL operations blocked behind wear leveling.
/// TODO(fxb/31379): Need Implementation of "TRIM" command for flash devices
///
/// Freezes can cause running 'iquery' to hang, so we just run iquery every second
/// and watch how long it takes to execute.  Most executions take less than 1 second
/// so 5 seconds is enough to tell that the system was probably wedged.
///
/// In addition, it provides functions for insight into whether a freeze happened,
/// which can be used for retrying failed tests.
class FreezeDetector {
  final Inspect inspect;

  bool _started = false;
  bool _isFrozen = false;
  bool _freezeHappened = false;
  Completer _c;

  Duration threshold = const Duration(seconds: 5);
  static const _workInterval = Duration(seconds: 1);

  Timer _worker;
  final _lastExecution = Stopwatch();

  FreezeDetector(this.inspect);

  void _workerHandler() async {
    _lastExecution.reset();

    // Start the query we use to detect a freeze
    final retrieveHub = inspect.retrieveHubEntries();

    // Schedule some work to happen if it doesn't complete in time.
    unawaited(retrieveHub.timeout(threshold, onTimeout: () {
      _isFrozen = true;
      _log.info('Freeze Start Detected ${DateTime.now()}');
      _freezeHappened = true;
      _c = Completer();
      return ['timeout'];
    }));

    // Wait for the query to finish.
    await retrieveHub;

    if (_isFrozen) {
      _log.info('Freeze End Detected ${DateTime.now()}');
      _isFrozen = false;
      _log.info('Freeze Duration ${_lastExecution.elapsed}');
      _c.complete();
    }

    final nextWork = _workInterval - _lastExecution.elapsed;

    if (_started) {
      _worker = Timer(nextWork, _workerHandler);
    }
  }

  void start() {
    _log.info('Starting FreezeDetector');
    _started = true;
    Timer.run(_workerHandler);
    _lastExecution.start();
  }

  void stop() async {
    if (!_started) {
      return;
    }
    _log.info('Stopping FreezeDetector');
    _started = false;
    _worker?.cancel();
  }

  Future<void> waitUntilUnfrozen() => _c?.future;

  bool isFrozen() => _isFrozen;
  bool freezeHappened() => _freezeHappened;

  void clearFreezeHappened() {
    _freezeHappened = false;
  }
}
