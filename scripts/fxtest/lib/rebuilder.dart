// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io';
import 'package:io/ansi.dart';

import 'package:fxtest/fxtest.dart';

Future<Process> fxCommandRun(String fx, String cmd, [List<String> args]) {
  return Process.start(fx, [cmd, ...?args], mode: ProcessStartMode.inheritStdio)
      .then((Process process) async {
    final _exitCode = await process.exitCode;
    if (_exitCode != 0) {
      throw FxRunException('fx $cmd $args', _exitCode);
    }
    return process;
  });
}

Future<Process> fxCommandRunWithIO(
  Function(TestEvent) eventSink,
  Stylizer stylizer,
  String fx,
  String cmd, [
  List<String> args,
]) {
  eventSink(
    TestInfo(stylizer('> fx $cmd', [green, styleBold])),
  );
  return Process.start(fx, [cmd, ...?args]).then((Process process) {
    process
      ..stdout.transform(utf8.decoder).listen(
            (event) => eventSink(
              TestInfo(
                event.toString(),
                requiresPadding: false,
              ),
            ),
          )
      ..stderr.transform(utf8.decoder).listen(
            (event) => eventSink(
              FatalError(
                event.toString(),
              ),
            ),
          );
    return process;
  });
}
