// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/tracing/src/trace/spec.h"

#include "gtest/gtest.h"

namespace tracing {

namespace measure {

bool operator==(const measure::EventSpec& lhs, const measure::EventSpec& rhs) {
  return lhs.name == rhs.name && lhs.category == rhs.category;
}

bool operator==(const measure::DurationSpec& lhs,
                const measure::DurationSpec& rhs) {
  return lhs.id == rhs.id && lhs.event == rhs.event;
}

bool operator==(const measure::TimeBetweenSpec& lhs,
                const measure::TimeBetweenSpec& rhs) {
  return lhs.id == rhs.id && lhs.first_event == rhs.first_event &&
         lhs.first_anchor == rhs.first_anchor &&
         lhs.second_event == rhs.second_event &&
         lhs.second_anchor == rhs.second_anchor;
}

}  // namespace measure

namespace {

TEST(Spec, DecodingErrors) {
  std::string json;
  Spec result;
  // Empty input.
  EXPECT_FALSE(DecodeSpec(json, &result));

  // Not an object.
  json = "[]";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "yes";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "4a";
  EXPECT_FALSE(DecodeSpec(json, &result));

  // Incorrect parameter types.
  json = "{\"app\": 42}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "{\"args\": \"many\"}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "{\"args\": [42]}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "{\"categories\": \"many\"}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "{\"categories\": [42]}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "{\"duration\": \"long\"}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json = "{\"measure\": \"yes\"}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json =
      "{\"measure\": ["
      "{\"type\": 42}"
      "]}";
  EXPECT_FALSE(DecodeSpec(json, &result));

  // Unknown measurement type.
  json =
      "{\"measure\": ["
      "{\"type\": \"unknown\"}"
      "]}";
  EXPECT_FALSE(DecodeSpec(json, &result));

  // Missing measurement params.
  json =
      "{\"measure\": ["
      "{\"type\": \"duration\"}"
      "]}";
  EXPECT_FALSE(DecodeSpec(json, &result));
  json =
      "{\"measure\": ["
      "{\"type\": \"time_between\"}"
      "]}";
  EXPECT_FALSE(DecodeSpec(json, &result));
}

TEST(Spec, DecodeEmpty) {
  std::string json = "{}";

  Spec result;
  ASSERT_TRUE(DecodeSpec(json, &result));
  EXPECT_EQ("", result.app);
  EXPECT_EQ(0u, result.duration_specs.size());
  EXPECT_EQ(0u, result.time_between_specs.size());
}

TEST(Spec, DecodeArgs) {
  std::string json = "{\"args\": [\"--flag\", \"positional\"]}";

  Spec result;
  ASSERT_TRUE(DecodeSpec(json, &result));
  EXPECT_EQ(std::vector<std::string>({"--flag", "positional"}), result.args);
}

TEST(Spec, DecodeCategories) {
  std::string json = "{\"categories\": [\"c1\", \"c2\"]}";

  Spec result;
  ASSERT_TRUE(DecodeSpec(json, &result));
  EXPECT_EQ(std::vector<std::string>({"c1", "c2"}), result.categories);
}

TEST(Spec, DecodeDuration) {
  std::string json = "{\"duration\": 42}";

  Spec result;
  ASSERT_TRUE(DecodeSpec(json, &result));
  EXPECT_EQ(ftl::TimeDelta::FromSeconds(42).ToNanoseconds(),
            result.duration.ToNanoseconds());
}

TEST(Spec, DecodeMeasureDuration) {
  std::string json =
      "{\"measure\":["
      "{\"type\":\"duration\","
      "\"event_name\":\"initialization\","
      "\"event_category\":\"bazinga\"},"
      "{\"type\":\"duration\","
      "\"event_name\":\"startup\","
      "\"event_category\":\"foo\"}"
      "]"
      "}";

  Spec result;
  ASSERT_TRUE(DecodeSpec(json, &result));
  EXPECT_EQ(2u, result.duration_specs.size());
  EXPECT_EQ(measure::DurationSpec({0u, {"initialization", "bazinga"}}),
            result.duration_specs[0]);
  EXPECT_EQ(measure::DurationSpec({1u, {"startup", "foo"}}),
            result.duration_specs[1]);
}

TEST(Spec, DecodeMeasureTimeBetween) {
  std::string json =
      "{\"measure\":["
      "{\"type\":\"time_between\","
      "\"first_event_name\":\"e1\","
      "\"first_event_category\":\"c1\","
      "\"first_event_anchor\":\"begin\","
      "\"second_event_name\":\"e2\","
      "\"second_event_category\":\"c2\","
      "\"second_event_anchor\":\"end\"}"
      "]"
      "}";

  Spec result;
  ASSERT_TRUE(DecodeSpec(json, &result));
  EXPECT_EQ(1u, result.time_between_specs.size());
  EXPECT_EQ(measure::TimeBetweenSpec({0u,
                                      {"e1", "c1"},
                                      measure::Anchor::Begin,
                                      {"e2", "c2"},
                                      measure::Anchor::End}),
            result.time_between_specs[0]);
}

}  // namespace

}  // namespace tracing
