// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_TEST_HERMETIC_AUDIO_TEST_H_
#define SRC_MEDIA_AUDIO_LIB_TEST_HERMETIC_AUDIO_TEST_H_

#include "src/media/audio/lib/test/hermetic_audio_environment.h"
#include "src/media/audio/lib/test/test_fixture.h"

namespace media::audio::test {

class HermeticAudioTest : public TestFixture {
 public:
  static void SetUpTestSuite();
  static void TearDownTestSuite();
  void SetUp() override;

  HermeticAudioEnvironment* environment() const {
    auto ptr = HermeticAudioTest::environment_.get();
    FXL_CHECK(ptr) << "No Environment; Did you forget to call SetUpTestSuite?";
    return ptr;
  }

 private:
  static std::unique_ptr<HermeticAudioEnvironment> environment_;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_LIB_TEST_HERMETIC_AUDIO_TEST_H_
