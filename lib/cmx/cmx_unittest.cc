// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/cmx/cmx.h"

#include <fcntl.h>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/fxl/files/path.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/json/json_parser.h"
#include "lib/pkg_url/fuchsia_pkg_url.h"
#include "rapidjson/document.h"

namespace component {
namespace {

class CmxMetadataTest : public ::testing::Test {
 protected:
  void ExpectFailedParse(const std::string& json, std::string expected_error) {
    std::string error;
    CmxMetadata cmx;
    json::JSONParser json_parser;
    std::string json_basename;
    EXPECT_FALSE(ParseFrom(&cmx, &json_parser, json, &json_basename));
    EXPECT_TRUE(json_parser.HasError());
    EXPECT_THAT(json_parser.error_str(), ::testing::HasSubstr(expected_error));
  }

  bool ParseFrom(CmxMetadata* cmx, json::JSONParser* json_parser,
                 const std::string& json, std::string* json_basename) {
    std::string json_path;
    if (!tmp_dir_.NewTempFileWithData(json, &json_path)) {
      return false;
    }
    *json_basename = files::GetBaseName(json_path);
    const int dirfd = open(tmp_dir_.path().c_str(), O_RDONLY);
    return cmx->ParseFromFileAt(dirfd, *json_basename, json_parser);
  }

  bool ParseFromDeprecatedRuntime(CmxMetadata* cmx,
                                  json::JSONParser* json_parser,
                                  const std::string& json,
                                  std::string* json_basename) {
    std::string json_path;
    if (!tmp_dir_.NewTempFileWithData(json, &json_path)) {
      return false;
    }
    *json_basename = files::GetBaseName(json_path);
    const int dirfd = open(tmp_dir_.path().c_str(), O_RDONLY);
    return cmx->ParseFromDeprecatedRuntimeFileAt(dirfd, *json_basename,
                                                 json_parser);
  }

 private:
  files::ScopedTempDir tmp_dir_;
};

TEST_F(CmxMetadataTest, ParseMetadata) {
  CmxMetadata cmx;
  json::JSONParser json_parser;
  const std::string json = R"JSON({
  "sandbox": {
      "dev": [ "class/input" ],
      "features": [ "feature_a" ],
      "services": [ "fuchsia.MyService" ]
  },
  "runner": "dart_runner",
  "facets": {
    "some_key": "some_value"
  },
  "other": "stuff"
  })JSON";
  std::string file_unused;
  EXPECT_TRUE(ParseFrom(&cmx, &json_parser, json, &file_unused))
      << json_parser.error_str();
  EXPECT_FALSE(json_parser.HasError());

  const auto& sandbox = cmx.sandbox_meta();
  EXPECT_FALSE(sandbox.IsNull());
  EXPECT_THAT(sandbox.dev(), ::testing::ElementsAre("class/input"));
  EXPECT_TRUE(sandbox.HasFeature("feature_a"));
  EXPECT_FALSE(sandbox.HasFeature("feature_b"));
  EXPECT_THAT(sandbox.services(), ::testing::ElementsAre("fuchsia.MyService"));

  EXPECT_FALSE(cmx.runtime_meta().IsNull());
  EXPECT_EQ(cmx.runtime_meta().runner(), "dart_runner");

  const auto& some_value = cmx.GetFacet("some_key");
  ASSERT_TRUE(some_value.IsString());
  EXPECT_EQ("some_value", std::string(some_value.GetString()));
  const auto& null_value = cmx.GetFacet("invalid");
  EXPECT_TRUE(null_value.IsNull());
}

TEST_F(CmxMetadataTest, ParseEmpty) {
  // No 'program' or 'runner'. Valid syntax, but empty.
  rapidjson::Value sandbox;
  std::string error;
  const std::string json = R"JSON(
  {
    "sandwich": { "ingredients": [ "bacon", "lettuce", "tomato" ] }
  }
  )JSON";
  CmxMetadata cmx;
  json::JSONParser json_parser;
  std::string file_unused;
  EXPECT_TRUE(ParseFrom(&cmx, &json_parser, json, &file_unused))
      << json_parser.error_str();
  // Missing "sandbox" defaults to empty.
  EXPECT_FALSE(cmx.sandbox_meta().IsNull());
  EXPECT_TRUE(cmx.runtime_meta().IsNull());
  EXPECT_TRUE(cmx.program_meta().IsBinaryNull());
  EXPECT_TRUE(cmx.program_meta().IsDataNull());
}

TEST_F(CmxMetadataTest, ParseFromDeprecatedRuntime) {
  rapidjson::Value sandbox;
  std::string error;
  const std::string json = R"JSON(
  { "runner": "dart_runner" }
  )JSON";
  CmxMetadata cmx;
  json::JSONParser json_parser;
  std::string file_unused;
  EXPECT_TRUE(
      ParseFromDeprecatedRuntime(&cmx, &json_parser, json, &file_unused))
      << json_parser.error_str();
  EXPECT_TRUE(cmx.sandbox_meta().IsNull());
  EXPECT_FALSE(cmx.runtime_meta().IsNull());
  EXPECT_EQ("dart_runner", cmx.runtime_meta().runner());
  EXPECT_TRUE(cmx.program_meta().IsBinaryNull());
  EXPECT_TRUE(cmx.program_meta().IsDataNull());
}

TEST_F(CmxMetadataTest, ParseWithErrors) {
  rapidjson::Value sandbox;
  std::string error;
  ExpectFailedParse(R"JSON({ ,,, })JSON", "Missing a name for object member.");
  ExpectFailedParse(R"JSON(3)JSON", "File is not a JSON object.");
  ExpectFailedParse(R"JSON({ "sandbox" : 3})JSON",
                    "'sandbox' is not an object.");
  ExpectFailedParse(R"JSON({ "sandbox" : {"dev": "notarray"} })JSON",
                    "'dev' in sandbox is not an array.");
  ExpectFailedParse(R"JSON({ "runner" : 3 })JSON", "'runner' is not a string.");
  ExpectFailedParse(R"JSON({ "program" : { "binary": 3 } })JSON",
                    "'binary' in program is not a string.");
}

}  // namespace
}  // namespace component
