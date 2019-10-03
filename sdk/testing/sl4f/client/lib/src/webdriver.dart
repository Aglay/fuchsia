// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io' as io;

import 'package:logging/logging.dart';
import 'package:sl4f/sl4f.dart';
import 'package:webdriver/sync_core.dart' show WebDriver;
import 'package:webdriver/sync_io.dart' as sync_io;

final _log = Logger('Webdriver');

/// `WebDriverConnector` is a utility for host-driven tests that control Chrome
/// contexts running on a remote device under test(DuT).  `WebDriverConnector`
/// vends `WebDriver` objects connected to remote Chrome instances.
/// Check the [webdriver package](https://pub.dev/documentation/webdriver/)
/// documentation for details on using `WebDriver`.
///
/// `WebDriverConnector` additionally starts an instance of the ChromeDriver
/// binary that runs locally on the test host.  `WebDriver` instances
/// communicate with ChromeDriver, which in turn communicates with Chrome
/// instances on the DuT.
///
/// TODO(satsukiu): Add e2e test for facade functionality
class WebDriverConnector {
  /// Relative path of chromedriver binary.
  final String _chromedriverPath;

  /// SL4F client.
  final Sl4f _sl4f;

  /// Helper for starting processes.
  final ProcessHelper _processHelper;

  /// Helper for instantiating WebDriver objects.
  final WebDriverHelper _webDriverHelper;

  /// A handle to the process running Chromedriver.
  io.Process _chromedriverProcess;

  /// The port Chromedriver is listening on.
  int _chromedriverPort;

  /// A mapping from an exposed port number on the DUT to an open WebDriver session.
  Map<int, WebDriverSession> _webDriverSessions;

  WebDriverConnector(String chromeDriverPath, Sl4f sl4f,
      {ProcessHelper processHelper, WebDriverHelper webDriverHelper})
      : _chromedriverPath = chromeDriverPath,
        _sl4f = sl4f,
        _processHelper = processHelper ?? ProcessHelper(),
        _webDriverHelper = webDriverHelper ?? WebDriverHelper(),
        _webDriverSessions = {};

  /// Starts ChromeDriver and enables DevTools for any future created Chrome
  /// contexts.  As this will not enable DevTools on any already opened
  /// contexts, `initialize` must be called prior to the instantiation of the
  /// Chrome context that needs to be driven.
  Future<void> initialize() async {
    await _startChromedriver();
    // TODO(satsukiu): return a nicer error, or don't fail if devtools is already enabled
    await _sl4f.request('webdriver_facade.EnableDevTools');
  }

  /// Stops Chromedriver and removes any connections that are still open.
  Future<void> tearDown() async {
    if (_chromedriverProcess != null) {
      _log.info('Stopping chromedriver');
      _chromedriverProcess.kill();
      await _chromedriverProcess.exitCode.timeout(Duration(seconds: 5),
          onTimeout: () {
        _log.warning('Chromedriver did not shut down, killing it.');
        _chromedriverProcess.kill(io.ProcessSignal.sigkill);
        return _chromedriverProcess.exitCode;
      });
      _chromedriverProcess = null;
    }

    for (final session in _webDriverSessions.entries) {
      await _sl4f.ssh.cancelPortForward(
          port: session.value.localPort, remotePort: session.key);
    }
    _webDriverSessions = {};
  }

  /// Get all nonEmpty Urls obtained from current _webDriverSessions.
  Iterable<String> get sessionsUrls => _webDriverSessions.values
      .map((session) => session.webDriver.currentUrl)
      .where((url) => url.isNotEmpty);

  /// Searches for Chrome contexts based on the host of the currently displayed
  /// page, and returns `WebDriver` connections to the found contexts.
  Future<List<WebDriver>> webDriversForHost(String host) async {
    _log.info('Finding webdrivers for $host');
    await _updateWebDriverSessions();

    return List.from(_webDriverSessions.values
        .where(
            (session) => Uri.parse(session.webDriver.currentUrl).host == host)
        .map((session) => session.webDriver));
  }

  /// Starts Chromedriver on the host.
  Future<void> _startChromedriver() async {
    if (_chromedriverProcess == null) {
      _chromedriverPort = await _sl4f.ssh.pickUnusedPort();

      final chromedriver =
          io.Platform.script.resolve(_chromedriverPath).toFilePath();
      final args = ['--port=$_chromedriverPort'];
      _chromedriverProcess = await _processHelper.start(chromedriver, args);
      _chromedriverProcess.stderr
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen((error) {
        _log.info('[Chromedriver] $error');
      });

      _chromedriverProcess.stdout
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen((log) {
        _log.info('[Chromedriver] $log');
      });
    }
  }

  /// Updates the set of open WebDriver connections.
  Future<void> _updateWebDriverSessions() async {
    final remotePortsResult =
        await _sl4f.request('webdriver_facade.GetDevToolsPorts');
    final remotePorts = Set.from(remotePortsResult['ports']);

    // Remove port forwarding for any ports that aren't open anymore.
    _webDriverSessions.removeWhere((remotePort, session) {
      if (!remotePorts.contains(remotePort)) {
        _sl4f.ssh
            .cancelPortForward(port: session.localPort, remotePort: remotePort);
        return true;
      }
      return false;
    });

    // Add new sessions for new ports.  For a given Chrome context listening on
    // port p on the DuT, we choose an unused local port x, and forward
    // localhost:x to DuT:p, and create a WebDriver instance pointing to localhost:x.
    for (final remotePort in remotePorts) {
      if (!_webDriverSessions.containsKey(remotePort)) {
        final localPort = await _sl4f.ssh.forwardPort(remotePort: remotePort);
        final webDriver =
            _webDriverHelper.createDriver(localPort, _chromedriverPort);

        _webDriverSessions.putIfAbsent(
            remotePort, () => WebDriverSession(localPort, webDriver));
      }
    }
  }
}

/// A representation of a `WebDriver` connection from a host device to a DUT.
class WebDriverSession {
  /// The local port forwarded to the DUT.
  final int localPort;

  /// The webdriver connection.
  final WebDriver webDriver;

  WebDriverSession(this.localPort, this.webDriver);
}

/// A wrapper around static dart:io Process methods.
class ProcessHelper {
  ProcessHelper();

  /// Start a new process.
  Future<io.Process> start(String cmd, List<String> args,
          {bool runInShell = false}) =>
      io.Process.start(cmd, args, runInShell: runInShell);
}

/// A wrapper around static WebDriver creation methods.
class WebDriverHelper {
  WebDriverHelper();

  /// Create a new WebDriver pointing to Chromedriver on the given uri and with
  /// given desired capabilities.
  WebDriver createDriver(int localPort, int chromedriverPort) {
    final chromeOptions = {'debuggerAddress': 'localhost:$localPort'};
    final capabilities = sync_io.Capabilities.chrome;
    capabilities[sync_io.Capabilities.chromeOptions] = chromeOptions;
    return sync_io.createDriver(
        desired: capabilities,
        uri: Uri.parse('http://localhost:$chromedriverPort'));
  }
}
