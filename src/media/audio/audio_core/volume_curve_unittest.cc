// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/volume_curve.h"

#include <fuchsia/media/cpp/fidl.h>

#include <gtest/gtest.h>

#include "src/media/audio/audio_core/mixer/gain.h"

namespace media::audio {
namespace {

TEST(VolumeCurveTest, ValidationRejectsInsufficientMappings) {
  auto result1 = VolumeCurve::FromMappings({});
  ASSERT_TRUE(result1.is_error());
  EXPECT_EQ(result1.error(), VolumeCurve::kLessThanTwoMappingsCannotMakeCurve);

  auto result2 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result2.is_error());
  EXPECT_EQ(result2.error(), VolumeCurve::kLessThanTwoMappingsCannotMakeCurve);
}

TEST(VolumeCurveTest, ValidationRejectsInsufficientDomain) {
  auto result1 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -10.0),
      VolumeCurve::VolumeMapping(0.5, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result1.is_error());
  EXPECT_EQ(result1.error(), VolumeCurve::kDomain0to1NotCovered);

  auto result2 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(0.2, -0.45),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result2.is_error());
  EXPECT_EQ(result2.error(), VolumeCurve::kDomain0to1NotCovered);
}

TEST(VolumeCurveTest, ValidationRejectsInsufficientRange) {
  auto result1 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -10.0),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, -1.0),
  });
  ASSERT_TRUE(result1.is_error());
  EXPECT_EQ(result1.error(), VolumeCurve::kRange0NotCovered);
}

TEST(VolumeCurveTest, ValidationRejectsNonIncreasingDomains) {
  auto result1 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -100.0),
      VolumeCurve::VolumeMapping(0.2, -34.0),
      VolumeCurve::VolumeMapping(0.2, -31.0),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result1.is_error());
  EXPECT_EQ(result1.error(), VolumeCurve::kNonIncreasingDomainIllegal);

  auto result2 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -100.0),
      VolumeCurve::VolumeMapping(0.2, -34.0),
      VolumeCurve::VolumeMapping(0.1, -31.0),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result2.is_error());
  EXPECT_EQ(result2.error(), VolumeCurve::kNonIncreasingDomainIllegal);
}

TEST(VolumeCurveTest, ValidationRejectsNonIncreasingRanges) {
  auto result1 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -2.0),
      VolumeCurve::VolumeMapping(0.2, -1.0),
      VolumeCurve::VolumeMapping(0.3, -10.0),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result1.is_error());
  EXPECT_EQ(result1.error(), VolumeCurve::kNonIncreasingRangeIllegal);

  auto result2 = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -2.0),
      VolumeCurve::VolumeMapping(0.1, -0.3),
      VolumeCurve::VolumeMapping(0.2, -0.3),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, Gain::kUnityGainDb),
  });
  ASSERT_TRUE(result2.is_error());
  EXPECT_EQ(result2.error(), VolumeCurve::kNonIncreasingRangeIllegal);
}

TEST(VolumeCurveTest, VolumeToDbBasic) {
  auto curve_result = VolumeCurve::FromMappings({
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MIN_VOLUME, -100.0),
      VolumeCurve::VolumeMapping(fuchsia::media::audio::MAX_VOLUME, Gain::kUnityGainDb),
  });

  ASSERT_TRUE(curve_result.is_ok());
  auto curve = curve_result.take_value();

  EXPECT_FLOAT_EQ(curve.VolumeToDb(fuchsia::media::audio::MIN_VOLUME), -100.0);
  EXPECT_FLOAT_EQ(curve.VolumeToDb(0.25), -75.0);
  EXPECT_FLOAT_EQ(curve.VolumeToDb(0.5), -50.0);
  EXPECT_FLOAT_EQ(curve.VolumeToDb(0.75), -25.0);
  EXPECT_FLOAT_EQ(curve.VolumeToDb(fuchsia::media::audio::MAX_VOLUME), Gain::kUnityGainDb);
}

TEST(VolumeCurveTest, DefaultCurves) {
  auto curve = VolumeCurve::Default();

  EXPECT_FLOAT_EQ(curve.VolumeToDb(fuchsia::media::audio::MIN_VOLUME),
                  fuchsia::media::audio::MUTED_GAIN_DB);
  EXPECT_FLOAT_EQ(curve.VolumeToDb(fuchsia::media::audio::MAX_VOLUME), Gain::kUnityGainDb);

  const auto middle = curve.VolumeToDb(0.5);
  EXPECT_GT(middle, fuchsia::media::audio::MUTED_GAIN_DB);
  EXPECT_LT(middle, Gain::kUnityGainDb);
}

TEST(VolumeCurveTest, DefaultCurveWithMinGainDb) {
  auto curve100 = VolumeCurve::DefaultForMinGain(-100.0);
  auto curve50 = VolumeCurve::DefaultForMinGain(-50.0);

  EXPECT_FLOAT_EQ(curve100.VolumeToDb(fuchsia::media::audio::MIN_VOLUME),
                  fuchsia::media::audio::MUTED_GAIN_DB);
  EXPECT_FLOAT_EQ(curve100.VolumeToDb(fuchsia::media::audio::MIN_VOLUME),
                  fuchsia::media::audio::MUTED_GAIN_DB);
  EXPECT_FLOAT_EQ(curve50.VolumeToDb(fuchsia::media::audio::MAX_VOLUME), Gain::kUnityGainDb);
  EXPECT_FLOAT_EQ(curve50.VolumeToDb(fuchsia::media::audio::MAX_VOLUME), Gain::kUnityGainDb);

  const auto middle100 = curve100.VolumeToDb(0.5);
  const auto middle50 = curve50.VolumeToDb(0.5);

  EXPECT_LT(middle100, middle50);
}

TEST(VolumeCurveTest, DefaultCurveWithMuteGainDoesNotAbort) {
  VolumeCurve::DefaultForMinGain(fuchsia::media::audio::MUTED_GAIN_DB);
}

}  // namespace
}  // namespace media::audio
