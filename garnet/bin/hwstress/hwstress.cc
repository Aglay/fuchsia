// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hwstress.h"

#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <stdio.h>

#include <complex>
#include <string>
#include <type_traits>
#include <utility>

#include "args.h"
#include "cpu_stress.h"
#include "flash_stress.h"
#include "light_stress.h"
#include "memory_stress.h"
#include "util.h"

namespace hwstress {

constexpr std::string_view kDefaultTemperatureSensorPath = "/dev/class/thermal/000";

constexpr uint64_t kDefaultFlashToTest = 16 * 1024 * 1024;

int Run(int argc, const char** argv) {
  // Parse arguments
  fitx::result<std::string, CommandLineArgs> result =
      ParseArgs(fbl::Span<const char* const>(argv, argc));
  if (result.is_error()) {
    fprintf(stderr, "Error: %s\n\n", result.error_value().c_str());
    PrintUsage();
    return 1;
  }
  const CommandLineArgs& args = result.value();

  // Print help and exit if requested.
  if (args.help) {
    PrintUsage();
    return 0;
  }

  // Calculate finish time.
  zx::duration duration = args.test_duration_seconds == 0
                              ? zx::duration::infinite()
                              : SecsToDuration(args.test_duration_seconds);

  // Calculate the amount of flash memory to test.
  uint64_t flash_to_test = kDefaultFlashToTest;
  if (args.mem_to_test_megabytes.has_value()) {
    flash_to_test = args.mem_to_test_megabytes.value() * 1024 * 1024;
  }

  // Attempt to create a hardware sensor.
  std::unique_ptr<TemperatureSensor> sensor =
      CreateSystemTemperatureSensor(kDefaultTemperatureSensorPath);
  if (sensor == nullptr) {
    sensor = CreateNullTemperatureSensor();
  }

  // Run the stress test.
  StatusLine status(args.verbose ? LogLevel::kVerbose : LogLevel::kNormal);
  bool success = false;
  switch (args.subcommand) {
    case StressTest::kCpu:
      success = StressCpu(&status, args, duration, sensor.get());
      break;
    case StressTest::kFlash:
      success = StressFlash(&status, args.fvm_path, flash_to_test);
      break;
    case StressTest::kLight:
      success = StressLight(&status, args, duration);
      break;
    case StressTest::kMemory:
      success = StressMemory(&status, args, duration, sensor.get());
      break;
  }

  return success ? 0 : 1;
}

}  // namespace hwstress
