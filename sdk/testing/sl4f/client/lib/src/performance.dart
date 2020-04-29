// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io' show File, Platform, Process, ProcessResult, WebSocket;

import 'package:logging/logging.dart';
import 'package:meta/meta.dart';
import 'package:path/path.dart' as path;

import 'dump.dart';
import 'sl4f_client.dart';
import 'storage.dart';
import 'trace_processing/metrics_spec.dart';
import 'trace_processing/trace_importing.dart';

String _removeSuffix(String string, String suffix) {
  if (!string.endsWith(suffix)) {
    throw ArgumentError('String "$string" does not end with "$suffix"');
  }
  return string.substring(0, string.length - suffix.length);
}

String _traceExtension({bool binary, bool compress}) {
  String extension = 'json';
  if (binary) {
    extension = 'fxt';
  }
  if (compress) {
    extension += '.gz';
  }
  return extension;
}

File _replaceExtension(File file, String newExtension) {
  String basePath = file.path;
  final firstExtension = path.extension(basePath);
  basePath = path.withoutExtension(basePath);
  if (firstExtension == '.gz') {
    basePath = path.withoutExtension(basePath);
  }
  return File('$basePath.$newExtension');
}

String _traceNameToTargetPath(String traceName, String extension) {
  return '/tmp/$traceName-trace.$extension';
}

// Chromium tracing tools requires only one "Compositor", "CrBrowserMain",
// "CrRendererMain" and "VizCompositorThread" appears in the tracing
// json file, so we need to remove these names in the Fuchsia trace files
// to avoid name collision.
//
// Note that this function mutates the value of argument |rootTraceObject|.
Map<String, dynamic> _renameChromiumProcessesInFuchsiaTrace(
    Map<String, dynamic> rootTraceObject) {
  if (!rootTraceObject.containsKey('systemTraceEvents') ||
      !rootTraceObject['systemTraceEvents'].containsKey('events')) {
    return rootTraceObject;
  }
  final List<Map<String, dynamic>> events =
      rootTraceObject['systemTraceEvents']['events'];
  for (final Map<String, dynamic> event in events) {
    if (event['ph'] == 't' && event.containsKey('name')) {
      final String name = event['name'];
      if (name == 'Compositor' ||
          name == 'CrBrowserMain' ||
          name == 'CrRendererMain' ||
          name == 'VizCompositorThread') {
        event['name'] = '${name}_Fuchsia';
      }
    }
  }
  return rootTraceObject;
}

final _log = Logger('Performance');

class Performance {
  // Names of environment variables used for tagging test results when
  // uploading to the Catapult performance dashboard.
  static const String _catapultDashboardMasterVarName =
      'CATAPULT_DASHBOARD_MASTER';
  static const String _catapultDashboardBotVarName = 'CATAPULT_DASHBOARD_BOT';
  static const String _buildbucketIdVarName = 'BUILDBUCKET_ID';
  static const String _buildCreateTimeVarName = 'BUILD_CREATE_TIME';

  final Sl4f _sl4f;
  final Dump _dump;

  /// Constructs a [Performance] object.
  Performance(this._sl4f, [Dump dump]) : _dump = dump ?? Dump();

  /// Closes the underlying HTTP client.
  ///
  /// This need not be called if the Sl4f client is closed instead.
  void close() {
    _sl4f.close();
  }

  Future<TraceSession> initializeTracing(
      {List<String> categories, int bufferSize}) async {
    _log.info('Performance: Initializing trace session');
    final params = {};
    if (categories != null) {
      params['categories'] = categories;
    }
    if (bufferSize != null) {
      params['buffer_size'] = bufferSize;
    }
    await _sl4f.request('tracing_facade.Initialize', params);
    return TraceSession(_sl4f, _dump);
  }

  /// Terminate any existing trace session without collecting trace data.
  Future<void> terminateExistingTraceSession() async {
    _log.info('Performance: Terminating any existing trace session');
    await _sl4f
        .request('tracing_facade.Terminate', {'results_destination': 'Ignore'});
  }

  /// Starts tracing for the given [duration].
  ///
  /// If [binary] is true, then the trace will be captured in Fuchsia Trace
  /// Format (by default, it is in Chrome JSON Format). If [compress] is true,
  /// the trace will be gzip-compressed. The trace output will be saved to a
  /// path implied by [traceName], [binary], and [compress], and can be
  /// retrieved later via [downloadTraceFile].
  Future<bool> trace(
      {@required Duration duration,
      @required String traceName,
      String categories,
      int bufferSize,
      bool binary = false,
      bool compress = false}) async {
    // Invoke `/bin/trace record --duration=$duration --categories=$categories
    // --output-file=$outputFile --buffer-size=$bufferSize` on the target
    // device via ssh.
    final durationSeconds = duration.inSeconds;
    String command = 'trace record --duration=$durationSeconds';
    if (categories != null) {
      command += ' --categories=$categories';
    }
    if (bufferSize != null) {
      command += ' --buffer-size=$bufferSize';
    }
    if (binary) {
      command += ' --binary';
    }
    if (compress) {
      command += ' --compress';
    }
    final String extension =
        _traceExtension(binary: binary, compress: compress);
    final outputFile = _traceNameToTargetPath(traceName, extension);
    if (outputFile != null) {
      command += ' --output-file=$outputFile';
    }
    final result = await _sl4f.ssh.run(command);
    return result.exitCode == 0;
  }

  /// Copies the trace file specified by [traceName] off of the target device,
  /// and then saves it to the dump directory.
  ///
  /// A [trace] call with the same [traceName], [binary], and [compress] must
  /// have successfully completed before calling [downloadTraceFile]. The trace
  /// file will be removed from the target device once it is downloaded.
  ///
  /// Returns the download trace [File].
  Future<File> downloadTraceFile(String traceName,
      {bool binary = false, bool compress = false}) async {
    _log.info('Performance: Downloading trace $traceName');
    final String extension =
        _traceExtension(binary: binary, compress: compress);
    final tracePath = _traceNameToTargetPath(traceName, extension);

    var response = await _sl4f
        .request('traceutil_facade.GetTraceFile', {'path': tracePath});
    List<int> contents = base64.decode(response['data']);
    while (response.containsKey('next_offset')) {
      response = await _sl4f.request('traceutil_facade.GetTraceFile',
          {'path': tracePath, 'offset': response['next_offset']});
      contents += base64.decode(response['data']);
    }

    await Storage(_sl4f).deleteFile(tracePath);

    return _dump.writeAsBytes('$traceName-trace', extension, contents);
  }

  /// Starts a Chrome trace from the given [webSocketUrl] with the default
  /// categories.
  ///
  /// [webSocketUrl] can be obtained from
  /// [Webdriver.webSocketDebuggerUrlsForHost]. Returns a WebSocket object that
  /// is to be passed to [stopChromeTrace] to stop and download the trace data.
  ///
  /// TODO(35714): Allow tracing users to specify categories to trace.
  Future<WebSocket> startChromeTrace(String webSocketUrl) async {
    final webSocket = await WebSocket.connect(webSocketUrl);
    _log.info('Starting chrome trace');
    webSocket.add(json.encode({
      'jsonrpc': '2.0',
      'method': 'Tracing.start',
      'params': {},
      'id': 1,
    }));
    return webSocket;
  }

  /// Stops a Chrome trace that was started by [startChromeTrace] and writes it
  /// to a file.
  ///
  /// Returns the file containing the trace data. Calling [stopChromeTrace] on
  /// the same [webSocket] twice will throw an error.
  Future<File> stopChromeTrace(WebSocket webSocket,
      {@required String traceName}) async {
    _log.info('Stopping and saving chrome trace');
    webSocket.add(json.encode({
      'jsonrpc': '2.0',
      'method': 'Tracing.end',
      'params': {},
      'id': 2,
    }));

    final traceEvents = [];

    await for (final content in webSocket) {
      final obj = json.decode(content);
      if (obj['method'] == 'Tracing.tracingComplete') {
        break;
      } else if (obj['method'] == 'Tracing.dataCollected') {
        traceEvents.addAll(obj['params']['value']);
      }
    }
    await webSocket.close();

    _log.info('Writing chrome trace to file');
    return _dump.writeAsBytes('$traceName-chrome-trace', 'json',
        utf8.encode(json.encode(traceEvents)));
  }

  /// Combine [fuchsiaTrace] and [chromeTrace] into a merged JSON-format trace.
  ///
  /// [fuchsiaTrace] must be a trace file in JSON format (not FXT).
  Future<File> mergeTraces(
      {@required File fuchsiaTrace,
      @required File chromeTrace,
      @required String traceName}) async {
    final fuchsiaTraceData = _renameChromiumProcessesInFuchsiaTrace(
        json.decode(await fuchsiaTrace.readAsString()));
    final chromeTraceData = json.decode(await chromeTrace.readAsString());

    final mergedTraceData = fuchsiaTraceData;
    mergedTraceData['traceEvents'].addAll(chromeTraceData);

    return _dump.writeAsBytes('$traceName-merged-trace', 'json',
        utf8.encode(json.encode(mergedTraceData)));
  }

  /// A helper function that runs a process with the given args.
  /// Required by the test to capture the parameters passed to [Process.run].
  ///
  /// Returns [true] if the process ran successufly, [false] otherwise.
  Future<bool> runProcess(String executablePath, List<String> args) async {
    _log.info('Performance: Running $executablePath ${args.join(" ")}');
    final ProcessResult results = await Process.run(executablePath, args);
    _log..info(results.stdout)..info(results.stderr);
    return results.exitCode == 0;
  }

  /// Convert the specified [traceFile] from fxt or fxt.gz to json or json.gz.
  ///
  /// In typical uses, [traceFile] should be the return value of a call to
  /// [downloadTraceFile].
  ///
  /// By default, this function guesses whether the input is compressed by
  /// examining [traceFile]'s extension. This can be overridden by passing a
  /// value for [compressedInput]. If [compressedOutput] is set to true, then
  /// this will produce a json.gz file instead of a json file.
  ///
  /// Returns the [File] generated by trace2json.
  Future<File> convertTraceFileToJson(String trace2jsonPath, File traceFile,
      {bool compressedInput, bool compressedOutput = false}) async {
    _log.info('Performance: Converting ${traceFile.absolute.path} to json');
    final String outputExtension =
        _traceExtension(binary: false, compress: compressedOutput);
    final File outputFile = _replaceExtension(traceFile, outputExtension);
    final args = [
      '--input-file=${traceFile.path}',
      '--output-file=${outputFile.path}',
    ];
    if (compressedInput ?? path.extension(traceFile.path) == '.gz') {
      args.add('--compressed-input');
    }
    if (compressedOutput) {
      args.add('--compressed-output');
    }
    final trace2json = Platform.script.resolve(trace2jsonPath).toFilePath();
    if (!await runProcess(trace2json, args)) {
      return null;
    }
    return outputFile;
  }

  /// Runs the provided [MetricsSpecSet] on the given [trace].
  /// It sets the ouptut file location to be the same as the source.
  /// It will also run the catapult converter if the [converterPath] was provided.
  ///
  /// The [converterPath] must be relative to the script path.
  ///
  /// [registry] defines the set of known metrics processors, which can be
  /// specified to allow processing of custom metrics.
  ///
  /// TODO(PT-216): Avoid explicitly passing the [converterPath].
  ///
  /// Returns the benchmark result [File] generated by the processor.
  Future<File> processTrace(MetricsSpecSet metricsSpecSet, File trace,
      {String converterPath,
      Map<String, MetricsProcessor> registry = defaultMetricsRegistry}) async {
    _log.info('Processing trace: ${trace.path}');
    final outputFileName =
        '${trace.parent.absolute.path}/${metricsSpecSet.testName}-benchmark.fuchsiaperf.json';

    final model = await createModelFromFile(trace);
    final List<Map<String, dynamic>> results = [];

    for (final metricsSpec in metricsSpecSet.metricsSpecs) {
      _log.info('Applying metricsSpec ${metricsSpec.name} to ${trace.path}');
      final testCaseResultss =
          processMetrics(model, metricsSpec, registry: registry);
      for (final testCaseResults in testCaseResultss) {
        results.add(testCaseResults.toJson(testSuite: metricsSpecSet.testName));
      }
    }

    File(outputFileName)
      ..createSync()
      ..writeAsStringSync(json.encode(results));

    File processedResultFile = File(outputFileName);
    _log.info('Processing trace completed.');
    if (converterPath != null) {
      await convertResults(
          converterPath, processedResultFile, Platform.environment);
    }
    return processedResultFile;
  }

  /// Send the given perf test results for upload to the Catapult Dashboard.
  ///
  /// This converts the results to Catapult format.  If uploading to Catapult is
  /// enabled, this puts the resulting file in a directory with a filename
  /// indicating that it should be uploaded.
  Future<void> convertResults(String converterPath, File result,
      Map<String, String> environment) async {
    _log.info('Converting the results into the catapult format');

    var master = environment[_catapultDashboardMasterVarName];
    var bot = environment[_catapultDashboardBotVarName];
    final buildbucketId = environment[_buildbucketIdVarName];
    final buildCreateTime = environment[_buildCreateTimeVarName];

    bool uploadEnabled = true;
    String logurl;
    int timestamp;
    if (master == null && bot == null) {
      _log.info(
          'convertResults: Infra env vars are not set; treating as a local run.');
      bot = 'local-bot';
      master = 'local-master';
      logurl = 'http://ci.example.com/build/300';
      timestamp = new DateTime.now().millisecondsSinceEpoch;
      // Disable uploading so that we don't accidentally upload with the
      // dummy values set here.
      uploadEnabled = false;
    } else if (master != null &&
        bot != null &&
        buildbucketId != null &&
        buildCreateTime != null) {
      logurl = 'https://ci.chromium.org/b/$buildbucketId';
      timestamp = int.parse(buildCreateTime);
    } else {
      throw ArgumentError(
          'Catapult-related infra env vars are not set consistently');
    }

    final resultsPath = result.absolute.path;
    // The infra recipe looks for the filename extension '.catapult_json',
    // so uploading to the Catapult performance dashboard is disabled if we
    // use a different extension.
    final catapultExtension =
        uploadEnabled ? '.catapult_json' : '.catapult_json_disabled';
    final outputFileName =
        _removeSuffix(resultsPath, '.fuchsiaperf.json') + catapultExtension;

    final List<String> args = [
      '--input',
      result.absolute.path,
      '--output',
      outputFileName,
      '--execution-timestamp-ms',
      timestamp.toString(),
      '--masters',
      master,
      '--log-url',
      logurl,
      '--bots',
      bot
    ];

    final converter = Platform.script.resolve(converterPath).toFilePath();

    if (!await runProcess(converter, args)) {
      throw AssertionError('Running catapult_converter failed');
    }
    _log.info('Conversion to catapult results format completed.'
        ' Output file: $outputFileName');
  }
}

class TraceSession {
  final Sl4f _sl4f;
  final Dump _dump;
  bool _closed;

  TraceSession(this._sl4f, this._dump) : _closed = false;

  bool get closed => _closed;

  /// Start tracing.
  Future<void> start() async {
    if (_closed) {
      throw StateError('Cannot start: Session already terminated');
    }
    _log.info('Tracing: starting trace');
    await _sl4f.request('tracing_facade.Start');
  }

  /// Stop tracing.
  Future<void> stop() async {
    if (_closed) {
      throw StateError('Cannot stop: Session already terminated');
    }
    _log.info('Tracing: stopping trace');
    await _sl4f.request('tracing_facade.Stop');
  }

  /// Terminate the trace session and download the trace data, returning a
  /// [File] object with the Fuchsia trace format data.
  ///
  /// After a call to [terminateAndDownload], further calls on the
  /// [TraceSession] object will throw a [StateError].
  Future<File> terminateAndDownload(String traceName) async {
    if (_closed) {
      throw StateError('Cannot terminate: Session already terminated');
    }
    _log.info('Tracing: terminating trace');
    final response = await _sl4f.request('tracing_facade.Terminate');
    _closed = true;
    final traceData = base64.decode(response['data']);
    return _dump.writeAsBytes('$traceName-trace', 'fxt', traceData);
  }
}
