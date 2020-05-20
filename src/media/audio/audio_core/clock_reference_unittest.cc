// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/clock_reference.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/clock.h>

#include <gtest/gtest.h>

#include "src/media/audio/lib/clock/clone_mono.h"

namespace media::audio {
namespace {

// Verify copy ctor and copy assignment
TEST(ClockReferenceTest, ClockRefsAreCopyable) {
  // These two clocks may be precisely in-sync, but they are not the same object.
  auto clock = clock::WritableCloneOfMonotonic();
  auto clock2 = clock::CloneOfMonotonic();

  auto clock_ref = ClockReference::MakeWritable(clock);

  ClockReference copied_clock_ref(clock_ref);
  EXPECT_EQ(clock_ref.get().get_handle(), copied_clock_ref.get().get_handle());

  auto assigned_clock_ref = ClockReference::MakeReadonly(clock2);
  EXPECT_NE(clock_ref.get().get_handle(), assigned_clock_ref.get().get_handle());

  assigned_clock_ref = clock_ref;
  EXPECT_EQ(clock_ref.get().get_handle(), assigned_clock_ref.get().get_handle());
}

// Verify operator bool and is_valid()
TEST(ClockReferenceTest, IsValid) {
  ClockReference default_ref;
  EXPECT_FALSE(default_ref);
  EXPECT_FALSE(default_ref.is_valid());

  zx::clock uninitialized;
  auto uninitialized_ref = ClockReference::MakeReadonly(uninitialized);
  EXPECT_FALSE(uninitialized_ref);
  EXPECT_FALSE(uninitialized_ref.is_valid());

  auto clock = clock::CloneOfMonotonic();
  auto clock_ref = ClockReference::MakeReadonly(clock);
  EXPECT_TRUE(clock_ref);
  EXPECT_TRUE(clock_ref.is_valid());
}

TEST(ClockReferenceTest, ClockCanSubsequentlyBeSet) {
  zx::clock future_mono_clone;
  auto clock_ref = ClockReference::MakeReadonly(future_mono_clone);

  // Uninitialized clock is not yet running. This will set the clock in motion.
  clock::CloneMonotonicInto(&future_mono_clone);

  auto time1 = clock_ref.Read();
  auto time2 = clock_ref.Read();
  EXPECT_LT(time1, time2);
}

}  // namespace
}  // namespace media::audio
