// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:test/test.dart';

import 'helpers.dart';

void main() {
  enableLoggingOutput();

  test('fuchsia.audio.mixer', () async {
    final helper = await PerfTestHelper.make();
    const resultsFile = '/tmp/perf_results.json';
    final result = await helper.sl4fDriver.ssh
        .run('/bin/audio_mixer_profiler --perftest-json=$resultsFile');
    expect(result.exitCode, equals(0));
    await helper.processResults(resultsFile);
  }, timeout: Timeout.none);
}
