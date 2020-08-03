// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "src/media/audio/audio_core/mixer/mixer.h"

namespace media::audio::mixer {
namespace {

class StubMixer : public Mixer {
 public:
  StubMixer() : Mixer(0, 0) {}

  bool Mix(float*, uint32_t, uint32_t*, const void*, uint32_t, int32_t*, bool) final {
    return false;
  }
};

TEST(BookkeepingTest, Defaults) {
  StubMixer mixer;
  auto& info = mixer.bookkeeping();

  EXPECT_EQ(info.step_size, Mixer::FRAC_ONE);
  EXPECT_EQ(info.rate_modulo, 0u);
  EXPECT_EQ(info.denominator, 0u);
  EXPECT_EQ(info.src_pos_modulo, 0u);

  EXPECT_EQ(info.next_dest_frame, 0);
  EXPECT_EQ(info.next_frac_source_frame, 0);
  EXPECT_EQ(info.next_src_pos_modulo, 0u);
  EXPECT_EQ(info.frac_source_error, 0);

  EXPECT_EQ(info.dest_frames_to_frac_source_frames.subject_time(), 0);
  EXPECT_EQ(info.dest_frames_to_frac_source_frames.reference_time(), 0);
  EXPECT_EQ(info.dest_frames_to_frac_source_frames.subject_delta(), 0u);
  EXPECT_EQ(info.dest_frames_to_frac_source_frames.reference_delta(), 1u);

  EXPECT_EQ(info.clock_mono_to_frac_source_frames.subject_time(), 0);
  EXPECT_EQ(info.clock_mono_to_frac_source_frames.reference_time(), 0);
  EXPECT_EQ(info.clock_mono_to_frac_source_frames.subject_delta(), 0u);
  EXPECT_EQ(info.clock_mono_to_frac_source_frames.reference_delta(), 1u);
}

// Upon Reset, Bookkeeping should clear position modulo and gain ramp. It should also clear its
// historical dest and source frame counters.
TEST(BookkeepingTest, Reset) {
  StubMixer mixer;
  auto& info = mixer.bookkeeping();

  info.rate_modulo = 5;
  info.denominator = 7;

  info.src_pos_modulo = 3u;

  info.next_dest_frame = 13;
  info.next_frac_source_frame = FractionalFrames<int64_t>(11);
  info.next_src_pos_modulo = 2;
  info.frac_source_error = FractionalFrames<int64_t>::FromRaw(-17);

  info.gain.SetSourceGainWithRamp(-42.0f, zx::sec(1),
                                  fuchsia::media::audio::RampType::SCALE_LINEAR);
  EXPECT_TRUE(info.gain.IsRamping());

  info.Reset();

  EXPECT_EQ(info.rate_modulo, 5u);
  EXPECT_EQ(info.denominator, 7u);

  EXPECT_EQ(info.src_pos_modulo, 0u);

  EXPECT_EQ(info.next_dest_frame, 13);
  EXPECT_EQ(info.next_frac_source_frame, FractionalFrames<int64_t>(11));
  EXPECT_EQ(info.next_src_pos_modulo, 2u);
  EXPECT_EQ(info.frac_source_error, FractionalFrames<int64_t>::FromRaw(-17));

  EXPECT_FALSE(info.gain.IsRamping());
}

// Reset with dest_frame: sets the running dest and frac_src position counters appropriately.
// next_frac_source_frame is set according to dest_to_frac_src transform, next_src_pos_modulo
// according to rate_modulo and denominator.
TEST(BookkeepingTest, ResetPositions) {
  StubMixer mixer;
  auto& info = mixer.bookkeeping();

  info.rate_modulo = 5;
  info.denominator = 7;
  info.dest_frames_to_frac_source_frames = TimelineFunction(TimelineRate(17u));

  // All these values will be overwritten
  info.next_dest_frame = -97;
  info.next_frac_source_frame = FractionalFrames<int64_t>(7);
  info.next_src_pos_modulo = 1u;
  info.frac_source_error = FractionalFrames<int64_t>::FromRaw(-777);

  info.ResetPositions(100);

  EXPECT_EQ(info.next_dest_frame, 100);
  EXPECT_EQ(info.frac_source_error, 0);

  // Calculated directly from the TimelineFunction
  EXPECT_EQ(info.next_frac_source_frame, FractionalFrames<int64_t>::FromRaw(1700));

  // Calculated from rate_modulo and deominator, starting at zero. (100*5)%7 = 3.
  EXPECT_EQ(info.next_src_pos_modulo, 3u);
}

// From current values, AdvanceRunningPositions advances running positions for dest, frac_source and
// frac_source_modulo by given dest frames, based on the step_size, rate_modulo and denominator.
TEST(BookkeepingTest, AdvanceRunningPositions) {
  StubMixer mixer;
  auto& info = mixer.bookkeeping();

  info.step_size = Mixer::FRAC_ONE + 2;
  info.rate_modulo = 2;
  info.denominator = 5;
  info.src_pos_modulo = 3;

  info.next_dest_frame = 2;
  info.next_frac_source_frame = FractionalFrames<int64_t>(3);
  info.next_src_pos_modulo = 1;
  info.frac_source_error = FractionalFrames<int64_t>::FromRaw(-17);

  info.AdvanceRunningPositionsBy(9);

  // These should be unchanged
  EXPECT_EQ(info.src_pos_modulo, 3u);
  EXPECT_EQ(info.frac_source_error, FractionalFrames<int64_t>::FromRaw(-17));

  // These should be updated
  EXPECT_EQ(info.next_dest_frame, 11u);
  // Starts at 3 with position modulo 1 (out of 5).
  // Advanced by 9 dest frames at step_size "1.002" with rate_modulo 2.
  // Position mod: expect 1 + (9 * 2) = 19, %5 becomes 3 subframes and position modulo 4.
  // frac_src: expect 3 + (9 * 1.002) frames (12 frames + 18 subframes), plus 3 subs from above.
  // Thus expect new running src position: 12 frames, 21 subframes, position modulo 4.
  EXPECT_EQ(info.next_frac_source_frame,
            FractionalFrames<int64_t>(12) + FractionalFrames<int64_t>::FromRaw(21));
  EXPECT_EQ(info.next_src_pos_modulo, 4u);
}

// Also validate AdvanceRunningPositions for negative offsets.
TEST(BookkeepingTest, NegativeAdvanceRunningPosition) {
  StubMixer mixer;
  auto& info = mixer.bookkeeping();

  info.step_size = Mixer::FRAC_ONE + 2;
  info.rate_modulo = 2;
  info.denominator = 5;

  info.next_dest_frame = 12;
  info.next_frac_source_frame = FractionalFrames<int64_t>(3);
  info.next_src_pos_modulo = 0;

  info.AdvanceRunningPositionsBy(-3);

  EXPECT_EQ(info.next_dest_frame, 9u);

  // frac_src_pos starts at 3 frames, 0 subframes, with position modulo 0 out of 5.
  // Advanced by -3 dest frames at a step_size of [1 frame + 2 subframes+ mod 2/5]
  // For -3 dest frames, this is a src advance of -3 frames, -6 subframes, -6/5 mod.
  // src_pos_mod was 0/5, plus -6/5, is now -6/5, but negative modulo must be reduced.
  // 0 subframes + mod -6/5 becomes -2 subframe + mod 4/5.
  //
  // frac_src advances by -3f, -8 subframes (-6-2) to become 0 frames -8 subframes.
  EXPECT_EQ(info.next_frac_source_frame, FractionalFrames<int64_t>::FromRaw(-8));
  EXPECT_EQ(info.next_src_pos_modulo, 4u);
}

}  // namespace
}  // namespace media::audio::mixer
