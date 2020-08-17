// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:logging/logging.dart';
import 'package:path/path.dart' as path;
import 'package:pkg/pkg.dart';
import 'package:pm/pm.dart';
import 'package:quiver/core.dart' show Optional;
import 'package:sl4f/sl4f.dart' as sl4f;
import 'package:test/test.dart';

// TODO(fxb/53292): update to use test size.
const _timeout = Timeout(Duration(minutes: 5));

Future<String> formattedHostAddress(sl4f.Sl4f sl4fDriver) async {
  final output = await sl4fDriver.ssh.run('echo \$SSH_CONNECTION');
  final hostAddress =
      output.stdout.toString().split(' ')[0].replaceAll('%', '%25');
  return '[$hostAddress]';
}

void main() {
  final log = Logger('package_manager_test');
  final pmPath = Platform.script.resolve('runtime_deps/pm').toFilePath();
  String hostAddress;
  String manifestPath;

  sl4f.Sl4f sl4fDriver;

  setUpAll(() async {
    Logger.root
      ..level = Level.ALL
      ..onRecord.listen((rec) => print('[${rec.level}]: ${rec.message}'));
    sl4fDriver = sl4f.Sl4f.fromEnvironment();
    await sl4fDriver.startServer();

    hostAddress = await formattedHostAddress(sl4fDriver);
    manifestPath = Platform.script
        .resolve('runtime_deps/package_manifest.json')
        .toFilePath();
  });

  tearDownAll(() async {
    await sl4fDriver.stopServer();
    sl4fDriver.close();
  });
  group('Package Manager', () {
    Optional<String> originalRewriteRule;
    Set<String> originalRepos;
    PackageManagerRepo repoServer;
    String testPackageName = 'cts-package-manager-sample-component';

    setUp(() async {
      repoServer = await PackageManagerRepo.initRepo(sl4fDriver, pmPath, log);

      // Gather the original package management settings before test begins.
      originalRepos = await getCurrentRepos(sl4fDriver);
      var ruleListResponse = await sl4fDriver.ssh.run('pkgctl rule list');
      expect(ruleListResponse.exitCode, 0);
      originalRewriteRule =
          getCurrentRewriteRule(ruleListResponse.stdout.toString());
    });
    tearDown(() async {
      if (!await resetPkgctl(sl4fDriver, originalRepos, originalRewriteRule)) {
        log.severe('Failed to reset pkgctl to default state');
      }
      if (repoServer != null) {
        repoServer
          ..kill()
          ..cleanup();
      }
    });
    test(
        'Test that creates a repository, deploys a package, and '
        'validates that the deployed package is visible from the server',
        () async {
      // Covers these commands (success cases only):
      //
      // Newly covered:
      // amberctl add_src -n <path> -f http://<host>:<port>/config.json
      // amberctl enable_src -n devhost
      // amberctl rm_src -n <name>
      // pkgctl repo
      // pm serve -repo=<path> -l :<port>
      await repoServer.setupServe('$testPackageName-0.far', manifestPath, []);
      final optionalPort = repoServer.getServePort();
      expect(optionalPort.isPresent, isTrue);
      final port = optionalPort.value;

      log.info('Getting the available packages');
      final curlResponse =
          await Process.run('curl', ['http://localhost:$port/targets.json']);

      expect(curlResponse.exitCode, 0);
      final curlOutput = curlResponse.stdout.toString();
      expect(curlOutput.contains('$testPackageName/0'), isTrue);

      // Typically, there is a pre-existing rule pointing to `devhost`, but it isn't
      // guaranteed. Record what the rule list is before we begin, and confirm that is
      // the rule list when we are finished.
      final originalRuleList = (await repoServer.pkgctlRuleList(
              'Recording the current rule list', 0))
          .stdout
          .toString();

      await repoServer.amberctlAddSrcNF(
          'Adding the new repository ${repoServer.getRepoPath()} as an update source with http://$hostAddress:$port/config.json',
          repoServer.getRepoPath(),
          'http://$hostAddress:$port/config.json',
          0);

      // Check that our new repo source is listed.
      var listSrcsOutput = (await repoServer.pkgctlRepo(
              'Running pkgctl repo to list sources', 0))
          .stdout
          .toString();
      String repoName = repoServer.getRepoPath().replaceAll('/', '_');
      String repoUrl = 'fuchsia-pkg://$repoName';
      expect(listSrcsOutput.contains(repoUrl), isTrue);

      var ruleListOutput = (await repoServer.pkgctlRuleList(
              'Confirm rule list points to $repoName', 0))
          .stdout
          .toString();
      expect(
          hasExclusivelyOneItem(ruleListOutput, 'host_replacement', repoName),
          isTrue);

      await repoServer.amberctlRmsrcN('Cleaning up temp repo', repoName, 0);

      log.info('Checking that $repoUrl is gone');
      listSrcsOutput = (await repoServer.pkgctlRepo(
              'Running pkgctl repo to list sources', 0))
          .stdout
          .toString();
      expect(listSrcsOutput.contains(repoUrl), isFalse);

      if (originalRewriteRule.isPresent) {
        await repoServer.amberctlEnablesrcN(
            'Re-enabling original repo source', originalRewriteRule.value, 0);
      }

      listSrcsOutput = (await repoServer.pkgctlRuleList(
              'Confirm rule list is back to its original set.', 0))
          .stdout
          .toString();
      expect(listSrcsOutput, originalRuleList);

      log.info(
          'Killing serve process and ensuring the output contains `[pm serve]`.');
      final killStatus = repoServer.kill();
      expect(killStatus, isTrue);

      var serveOutputBuilder = StringBuffer();
      var serveProcess = repoServer.getServeProcess();
      expect(serveProcess.isPresent, isTrue);
      await serveProcess.value.stdout.transform(utf8.decoder).listen((data) {
        serveOutputBuilder.write(data);
      }).asFuture();
      final serveOutput = serveOutputBuilder.toString();
      // Ensuring that `[pm serve]` appears in the output because the `-q` flag
      // wasn't used in the serve command.
      expect(serveOutput.contains('[pm serve]'), isTrue);
    });
    test(
        'Test that creates a repository, deploys a package, and '
        'validates that the deployed package is visible from the server. '
        'Then make sure `pm serve` output is quiet.', () async {
      // Covers these commands (success cases only):
      //
      // Newly covered:
      // pm serve -repo=<path> -l :<port> -q
      //
      // Previously covered:
      // amberctl add_src -n <path> -f http://<host>:<port>/config.json
      await repoServer
          .setupServe('$testPackageName-0.far', manifestPath, ['-q']);
      final optionalPort = repoServer.getServePort();
      expect(optionalPort.isPresent, isTrue);
      final port = optionalPort.value;

      log.info('Getting the available packages');
      final curlResponse =
          await Process.run('curl', ['http://localhost:$port/targets.json']);

      expect(curlResponse.exitCode, 0);
      final curlOutput = curlResponse.stdout.toString();
      expect(curlOutput.contains('$testPackageName/0'), isTrue);

      await repoServer.amberctlAddSrcNF(
          'Adding the new repository as an update source with http://$hostAddress:$port',
          repoServer.getRepoPath(),
          'http://$hostAddress:$port/config.json',
          0);

      log.info(
          'Killing serve process and ensuring the output does not contain `[pm serve]`.');
      final killStatus = repoServer.kill();
      expect(killStatus, isTrue);

      var serveOutputBuilder = StringBuffer();
      var serveProcess = repoServer.getServeProcess();
      expect(serveProcess.isPresent, isTrue);
      await serveProcess.value.stdout.transform(utf8.decoder).listen((data) {
        serveOutputBuilder.write(data);
      }).asFuture();
      final serveOutput = serveOutputBuilder.toString();
      // The `-q` flag was given to `pm serve`, so there should be no serve output.
      expect(serveOutput.contains('[pm serve]'), isFalse);
    });
    test('Test amberctl default name behavior when no name is given.',
        () async {
      // Covers these commands (success cases only):
      //
      // Newly covered:
      // amberctl add_src -f http://<host>:<port>/config.json
      //
      // Previously covered:
      // pm serve -repo=<path> -l :<port>
      // pkgctl repo
      await repoServer.setupServe('$testPackageName-0.far', manifestPath, []);
      final optionalPort = repoServer.getServePort();
      expect(optionalPort.isPresent, isTrue);
      final port = optionalPort.value;

      await repoServer.amberctlAddSrcF(
          'Adding the new repository as an update source with http://$hostAddress:$port',
          'http://$hostAddress:$port/config.json',
          0);

      var listSrcsOutput = (await repoServer.pkgctlRepo(
              'Running pkgctl repo to list sources', 0))
          .stdout
          .toString();

      log.info('Running pkgctl repo to list sources');
      String repoName = 'http://$hostAddress:$port';
      // Remove the `%25ethp0003` from
      // http://[fe80::9813:f1ff:fe9b:c411%25ethp0003]:38729
      // (for example)
      RegExp(r'(%\w+)]').allMatches(repoName).forEach((match) {
        String str = match.group(1);
        repoName = repoName.replaceAll(str, '');
      });
      // Clean up name to match what amberctl will do.
      repoName = repoName
          .replaceAll('/', '_')
          .replaceAll(':', '_')
          .replaceAll('[', '_')
          .replaceAll(']', '_');

      log.info('Checking repo name is $repoName');
      expect(listSrcsOutput.contains(repoName), isTrue);
    });
    test('Test `pm serve` writes its port number to a given file path.',
        () async {
      // Covers these commands (success cases only):
      //
      // Newly covered:
      // pm serve -repo=<path> -l :<port> -f <path to export port number>
      await repoServer.setupRepo('$testPackageName-0.far', manifestPath);
      final portFilePath = path.join(repoServer.getRepoPath(), 'port_file.txt');

      await repoServer.pmServeRepoLExtra(['-f', '$portFilePath']);
      final optionalPort = repoServer.getServePort();
      expect(optionalPort.isPresent, isTrue);
      final port = optionalPort.value;

      // Wait long enough for the serve process to come up.
      log.info('Getting the available packages');
      final curlResponse =
          await Process.run('curl', ['http://localhost:$port/targets.json']);
      expect(curlResponse.exitCode, 0);

      log.info('Checking that $portFilePath was generated with content: $port');
      String fileContents = (await File(portFilePath).readAsString()).trim();
      expect(int.parse(fileContents), port);
    });
    test(
        'Test `amberctl add_repo_cfg` does not set a rewrite rule to use the '
        'added repo.', () async {
      // Covers these commands (success cases only):
      //
      // Newly covered:
      // amberctl add_repo_cfg -n <path> -f http://<host>:<port>/config.json
      //
      // Previously covered:
      // pkgctl repo
      // pm serve -repo=<path> -l :<port>
      await repoServer.setupServe('$testPackageName-0.far', manifestPath, []);
      final optionalPort = repoServer.getServePort();
      expect(optionalPort.isPresent, isTrue);
      final port = optionalPort.value;

      final originalRuleList = (await repoServer.pkgctlRuleList(
              'Recording the current rule list', 0))
          .stdout
          .toString();

      await repoServer.amberctlAddrepocfgNF(
          'Adding the new repository as an update source with http://$hostAddress:$port',
          'http://$hostAddress:$port/config.json',
          0);
      String repoName = repoServer.getRepoPath().replaceAll('/', '_');
      String repoUrl = 'fuchsia-pkg://$repoName';

      var listSrcsOutput = (await repoServer.pkgctlRepo(
              'Running pkgctl repo to list sources', 0))
          .stdout
          .toString();
      log.info('list sources: $listSrcsOutput, expect: $repoUrl');
      expect(listSrcsOutput.contains(repoUrl), isTrue);

      var listRulesOutput = (await repoServer.pkgctlRuleList(
              'Confirm rule list is NOT updated to point to $repoName', 0))
          .stdout
          .toString();
      expect(listRulesOutput.contains(repoName), isFalse);
      expect(listRulesOutput, originalRuleList);
    });
  }, timeout: _timeout);
}
