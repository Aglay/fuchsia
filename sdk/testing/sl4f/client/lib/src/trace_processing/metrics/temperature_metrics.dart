// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:logging/logging.dart';

import '../metrics_results.dart';
import '../time_delta.dart';
import '../trace_model.dart';
import 'common.dart';

final _log = Logger('TemperatureMetricsProcessor');

List<TestCaseResults> temperatureMetricsProcessor(
    Model model, Map<String, dynamic> extraArgs) {
  final duration = getTotalTraceDuration(model);
  if (duration < TimeDelta.fromMilliseconds(10100)) {
    _log.info(
        'Trace duration (${duration.toMilliseconds()} milliseconds) is too short to provide temperature information');
    return [];
  }
  final temperatureReadings = getArgValuesFromEvents<num>(
          filterEventsTyped<CounterEvent>(getAllEvents(model),
              category: 'system_metrics', name: 'temperature'),
          'temperature')
      .map((t) => t.toDouble());

  _log.info(
      'Average temperature reading: ${computeMean(temperatureReadings)} degree Celsius');
  return [
    TestCaseResults(
        'Device temperature', Unit.count, temperatureReadings.toList()),
  ];
}
