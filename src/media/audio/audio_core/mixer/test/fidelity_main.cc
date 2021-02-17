// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "lib/syslog/cpp/log_settings.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/test/test_settings.h"
#include "src/media/audio/audio_core/mixer/test/audio_result.h"
#include "src/media/audio/audio_core/mixer/test/frequency_set.h"
#include "src/media/audio/audio_core/mixer/test/mixer_tests_recap.h"
#include "src/media/audio/lib/logging/logging.h"

int main(int argc, char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  if (!fxl::SetTestSettings(command_line)) {
    return EXIT_FAILURE;
  }

  syslog::SetTags({"audio_fidelity_tests"});

  // --full  Measure across the full frequency spectrum; display full results in tabular format.
  // --recap Display summary fidelity results.
  // --dump  Display full-spectrum results in importable format.
  //         (This flag is used when updating AudioResult kPrev arrays.)
  //
  bool show_full_frequency_results = command_line.HasOption("full");
  bool show_summary_results = command_line.HasOption("recap");
  bool dump_threshold_values = command_line.HasOption("dump");

  media::audio::test::FrequencySet::UseFullFrequencySet =
      (show_full_frequency_results || dump_threshold_values);

  testing::InitGoogleTest(&argc, argv);

  int result = RUN_ALL_TESTS();

  if (show_full_frequency_results || show_summary_results) {
    media::audio::test::MixerTestsRecap::PrintFidelityResultsSummary();
  }
  if (dump_threshold_values) {
    media::audio::test::AudioResult::DumpThresholdValues();
  }

  return result;
}
