// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/json/json_parser.h"

#include <fcntl.h>
#include <stdio.h>
#include <string>

#include "gtest/gtest.h"
#include "lib/fxl/files/path.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/strings/concatenate.h"
#include "lib/fxl/strings/string_printf.h"
#include "third_party/rapidjson/rapidjson/document.h"

namespace json {
namespace {

class JSONParserTest : public ::testing::Test {
 protected:
  // ExpectFailedParse() will replace '$0' with the JSON filename, if present.
  void ExpectFailedParse(JSONParser* parser, const std::string& json,
                         std::string expected_error) {
    const std::string json_file = NewJSONFile(json);
    std::string error;
    EXPECT_FALSE(ParseFromFile(parser, json_file, &error));
    // TODO(DX-338): Use strings/substitute.h once that actually exists in fxl.
    size_t pos;
    while ((pos = expected_error.find("$0")) != std::string::npos) {
      expected_error.replace(pos, 2, json_file);
    }
    EXPECT_EQ(error, expected_error);
  }

  bool ParseFromFile(JSONParser* parser, const std::string& file,
                     std::string* error) {
    rapidjson::Document document = parser->ParseFromFile(file);
    if (!parser->HasError()) {
      InterpretDocument(parser, document);
    }
    *error = parser->error_str();
    return !parser->HasError();
  }

  bool ParseFromFileAt(JSONParser* parser, int dirfd, const std::string& file,
                       std::string* error) {
    rapidjson::Document document = parser->ParseFromFileAt(dirfd, file);
    if (!parser->HasError()) {
      InterpretDocument(parser, document);
    }
    *error = parser->error_str();
    return !parser->HasError();
  }

  std::string NewJSONFile(const std::string& json) {
    std::string json_file;
    if (!tmp_dir_.NewTempFileWithData(json, &json_file)) {
      return "";
    }
    return json_file;
  }

  void InterpretDocument(JSONParser* parser,
                         const rapidjson::Document& document) {
    if (!document.IsObject()) {
      parser->ReportError("Document is not an object.");
      return;
    }

    auto prop1 = document.FindMember("prop1");
    if (prop1 == document.MemberEnd()) {
      parser->ReportError("missing prop1");
    } else if (!prop1->value.IsString()) {
      parser->ReportError("prop1 has wrong type");
    }

    auto prop2 = document.FindMember("prop2");
    if (prop2 == document.MemberEnd()) {
      parser->ReportError("missing prop2");
    } else if (!prop2->value.IsInt()) {
      parser->ReportError("prop2 has wrong type");
    }
  }

  files::ScopedTempDir tmp_dir_;
};

TEST_F(JSONParserTest, ReadInvalidFile) {
  const std::string invalid_path =
      fxl::StringPrintf("%s/does_not_exist", tmp_dir_.path().c_str());
  std::string error;
  JSONParser parser;
  EXPECT_FALSE(ParseFromFile(&parser, invalid_path, &error));
  EXPECT_EQ(error,
            fxl::StringPrintf("Failed to read file: %s", invalid_path.c_str()));
}

TEST_F(JSONParserTest, ParseWithErrors) {
  std::string json;

  // One error, in parsing.
  {
    const std::string json = R"JSON({
  "prop1": "missing closing quote,
  "prop2": 42
  })JSON";
    JSONParser parser;
    ExpectFailedParse(&parser, json, "$0:2:35: Invalid encoding in string.");
  }

  // Multiple errors, after parsing.
  {
    const std::string json = R"JSON({
  "prop2": "wrong_type"
  })JSON";
    JSONParser parser;
    ExpectFailedParse(&parser, json,
                      "$0: missing prop1\n$0: prop2 has wrong type");
  }
}

TEST_F(JSONParserTest, ParseFromString) {
  const std::string json = R"JSON({
  "prop1": "missing closing quote
  })JSON";
  JSONParser parser;
  parser.ParseFromString(json, "test_file");
  EXPECT_TRUE(parser.HasError());
  EXPECT_EQ(parser.error_str(),
            "test_file:2:34: Invalid encoding in string.");
}

TEST_F(JSONParserTest, ParseTwice) {
  std::string json;
  JSONParser parser;

  // Two failed parses. Errors should accumulate.
  json = R"JSON({
  "prop1": invalid_value,
  })JSON";
  parser.ParseFromString(json, "test_file");

  json = R"JSON({
  "prop1": "missing closing quote
  })JSON";
  parser.ParseFromString(json, "test_file");

  EXPECT_TRUE(parser.HasError());
  EXPECT_EQ(parser.error_str(),
            "test_file:2:12: Invalid value.\n"
            "test_file:2:34: Invalid encoding in string.");
}

TEST_F(JSONParserTest, ParseValid) {
  const std::string json = R"JSON({
  "prop1": "foo",
  "prop2": 42
  })JSON";
  const std::string file = NewJSONFile(json);
  std::string error;
  JSONParser parser;
  EXPECT_TRUE(ParseFromFile(&parser, file, &error));
  EXPECT_EQ("", error);
}

TEST_F(JSONParserTest, ParseFromFileAt) {
  const std::string json = R"JSON({
  "prop1": "foo",
  "prop2": 42
  })JSON";
  const std::string file = NewJSONFile(json);
  const std::string basename = files::GetBaseName(file);
  const int dirfd = open(tmp_dir_.path().c_str(), O_RDONLY);
  ASSERT_GT(dirfd, 0);

  std::string error;
  JSONParser parser;
  EXPECT_TRUE(ParseFromFileAt(&parser, dirfd, basename, &error));
  EXPECT_EQ("", error);
}

}  // namespace
}  // namespace json
