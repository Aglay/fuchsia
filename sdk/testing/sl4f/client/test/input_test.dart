// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:math';
import 'package:mockito/mockito.dart';
import 'package:test/test.dart';
import 'package:sl4f/sl4f.dart';

class MockSl4f extends Mock implements Sl4f {}

void main(List<String> args) {
  MockSl4f sl4f;

  Duration duration = const Duration(milliseconds: 300);
  setUp(() {
    sl4f = MockSl4f();
  });

  group('without constructor default rotation', () {
    Input input;
    setUp(() {
      input = Input(sl4f);
    });

    test('input rotates 0 degrees', () async {
      await input.swipe(Point<int>(0, 0), Point<int>(1000, 1000));
      verify(sl4f.request('input_facade.Swipe', {
        'x0': 0,
        'y0': 0,
        'x1': 1000,
        'y1': 1000,
        'duration': duration.inMilliseconds,
      }));
    });

    test('input rotates 90 degrees', () async {
      await input.swipe(Point<int>(0, 0), Point<int>(1000, 1000),
          screenRotation: Rotation.degrees90);
      verify(sl4f.request('input_facade.Swipe', {
        'x0': 1000,
        'y0': 0,
        'x1': 0,
        'y1': 1000,
        'duration': duration.inMilliseconds,
      }));
    });

    test('input rotates 180 degrees', () async {
      await input.tap(Point<int>(0, 0), screenRotation: Rotation.degrees180);
      verify(sl4f.request('input_facade.Tap', {
        'x': 1000,
        'y': 1000,
      })).called(1);
    });

    test('input rotates 270 degrees', () async {
      await input.tap(Point<int>(0, 0), screenRotation: Rotation.degrees270);
      verify(sl4f.request('input_facade.Tap', {
        'x': 0,
        'y': 1000,
      })).called(1);
    });

    test('input multiple taps', () async {
      await input.tap(Point<int>(500, 500), tapEventCount: 10, duration: 100);
      verify(sl4f.request('input_facade.Tap', {
        'x': 500,
        'y': 500,
        'tap_event_count': 10,
        'duration': 100,
      })).called(1);
    });
  });

  test('input rotates with constructor default', () async {
    await Input(sl4f, Rotation.degrees270).tap(Point<int>(0, 0));
    verify(sl4f.request('input_facade.Tap', {
      'x': 0,
      'y': 1000,
    })).called(1);
  });
}
