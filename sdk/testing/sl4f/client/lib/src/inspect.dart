// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';

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

  Duration threshold = const Duration(seconds: 5);
  static const _workInterval = Duration(seconds: 1);
  static const _updateInterval = Duration(milliseconds: 100);

  Timer _checker;
  Timer _worker;
  final _lastExecution = Stopwatch();

  FreezeDetector(this.inspect);

  void _workerHandler() async {
    _lastExecution.reset();
    await inspect.retrieveHubEntries();
    if (_started) {
      _worker = new Timer(_workInterval, _workerHandler);
    }
  }

  void _timerHandler(timer) async {
    final checkTime = DateTime.now();
    final checkDuration = _lastExecution.elapsed;

    if (checkDuration > threshold) {
      if (!_isFrozen) {
        _isFrozen = true;
        _log.info('Freeze Start Detected $checkTime');
        _freezeHappened = true;
      }
    } else {
      if (_isFrozen) {
        _log.info('Freeze End Detected $checkTime');
        _isFrozen = false;
        _log.info('Freeze Duration $checkDuration');
      }
    }
  }

  void start() {
    _log.info('Starting FreezeDetector');
    _started = true;
    _worker = Timer(_workInterval, _workerHandler);
    _lastExecution.start();
    _checker = Timer.periodic(_updateInterval, _timerHandler);
  }

  void stop() async {
    if (!_started) {
      return;
    }
    _log.info('Stopping FreezeDetector');
    _checker?.cancel();
    _started = false;
    _worker?.cancel();
  }

  Future<void> waitUntilUnfrozen() async {
    while (_isFrozen) {
      await Future.delayed(_updateInterval);
    }
  }

  bool isFrozen() => _isFrozen;
  bool freezeHappened() => _freezeHappened;

  void clearFreezeHappened() {
    _freezeHappened = false;
  }
}
