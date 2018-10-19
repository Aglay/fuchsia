// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/cmx/cmx.h"

#include <trace/event.h>
#include <algorithm>
#include <regex>
#include <sstream>
#include <string>

#include "garnet/lib/pkg_url/fuchsia_pkg_url.h"
#include "lib/fxl/strings/substitute.h"
#include "rapidjson/document.h"

namespace component {

constexpr char kSandbox[] = "sandbox";
constexpr char kProgram[] = "program";
constexpr char kFacets[] = "facets";

CmxMetadata::CmxMetadata() = default;
CmxMetadata::~CmxMetadata() = default;

bool CmxMetadata::ParseFromFileAt(int dirfd, const std::string& file,
                                  json::JSONParser* json_parser) {
  TRACE_DURATION("cmx", "CmxMetadata::ParseFromFileAt", "file", file);
  rapidjson::Document document = json_parser->ParseFromFileAt(dirfd, file);
  if (json_parser->HasError()) {
    return false;
  }
  if (!document.IsObject()) {
    json_parser->ReportError("File is not a JSON object.");
    return false;
  }
  ParseSandboxMetadata(document, json_parser);
  runtime_meta_.ParseFromDocument(document, json_parser);
  ParseProgramMetadata(document, json_parser);
  ParseFacetsMetadata(document, json_parser);
  return !json_parser->HasError();
}

bool CmxMetadata::ParseFromDeprecatedRuntimeFileAt(
    int dirfd, const std::string& file, json::JSONParser* json_parser) {
  rapidjson::Document document = json_parser->ParseFromFileAt(dirfd, file);
  if (json_parser->HasError()) {
    return false;
  }
  if (!document.IsObject()) {
    json_parser->ReportError("File is not a JSON object.");
    return false;
  }
  runtime_meta_.ParseFromDocument(document, json_parser);
  return !json_parser->HasError();
}

// static
std::string CmxMetadata::GetDefaultComponentCmxPath(
    const FuchsiaPkgUrl& package_resolved_url) {
  return fxl::Substitute("meta/$0.cmx", package_resolved_url.package_name());
}

// static
std::string CmxMetadata::GetDefaultComponentName(
    const FuchsiaPkgUrl& package_resolved_url) {
  return package_resolved_url.package_name();
}

void CmxMetadata::ParseSandboxMetadata(const rapidjson::Document& document,
                                       json::JSONParser* json_parser) {
  auto sandbox = document.FindMember(kSandbox);
  if (sandbox == document.MemberEnd()) {
    // Valid syntax, but no value. Pass empty object.
    rapidjson::Value sandbox_obj = rapidjson::Value(rapidjson::kObjectType);
    sandbox_meta_.Parse(sandbox_obj, json_parser);
  } else if (!sandbox->value.IsObject()) {
    json_parser->ReportError("'sandbox' is not an object.");
    return;
  } else {
    sandbox_meta_.Parse(sandbox->value, json_parser);
  }
}

void CmxMetadata::ParseProgramMetadata(const rapidjson::Document& document,
                                       json::JSONParser* json_parser) {
  auto program = document.FindMember(kProgram);
  if (program == document.MemberEnd()) {
    // Valid syntax, but no value.
    return;
  }
  if (!program->value.IsObject()) {
    json_parser->ReportError("'program' is not an object.");
    return;
  }
  program_meta_.Parse(program->value, json_parser);
}

void CmxMetadata::ParseFacetsMetadata(const rapidjson::Document& document,
                                      json::JSONParser* json_parser) {
  auto facets = document.FindMember(kFacets);
  if (facets == document.MemberEnd()) {
    // Valid syntax, but no value.
    return;
  }
  facets_meta_.Parse(facets->value, json_parser);
}

}  // namespace component
