// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/component_id_index.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/assert.h>

#include <unordered_set>

#include <rapidjson/document.h>

#include "src/lib/files/file.h"

namespace component {
namespace {
const char kIndexFilePath[] = "component_id_index";

bool IsValidInstanceId(const std::string& instance_id) {
  // * 256-bits encoded in base16 = 64 characters
  //   - 1 char to represent 4 bits.
  if (instance_id.length() != 64) {
    return false;
  }
  for (size_t i = 0; i < 64; i++) {
    const auto& c = instance_id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

fit::result<std::pair<Moniker, ComponentIdIndex::InstanceId>, ComponentIdIndex::Error> ParseEntry(
    const rapidjson::Value& entry) {
  // Entry must be an object.
  if (!entry.IsObject()) {
    FX_LOGS(ERROR) << "Entry must be an object.";
    return fit::error(ComponentIdIndex::Error::INVALID_SCHEMA);
  }

  // `instance_id` is a required string.
  if (!entry.HasMember("instance_id") || !entry["instance_id"].IsString()) {
    FX_LOGS(ERROR) << "instance_id is a required string.";
    return fit::error(ComponentIdIndex::Error::INVALID_SCHEMA);
  }

  // `instance_id` must be a valid format.
  if (!IsValidInstanceId(entry["instance_id"].GetString())) {
    FX_LOGS(ERROR) << "instance_id must be valid format.";
    return fit::error(ComponentIdIndex::Error::INVALID_INSTANCE_ID);
  }

  // `appmgr_moniker` is a required object.
  if (!entry.HasMember("appmgr_moniker") || !entry["appmgr_moniker"].IsObject()) {
    FX_LOGS(ERROR) << "appmgr_moniker must be valid object.";
    return fit::error(ComponentIdIndex::Error::INVALID_MONIKER);
  }

  const auto& appmgr_moniker = entry["appmgr_moniker"];
  // `url` is a required string.
  if (!appmgr_moniker.HasMember("url") || !appmgr_moniker["url"].IsString()) {
    FX_LOGS(ERROR) << "appmgr_moniker.url is a required string.";
    return fit::error(ComponentIdIndex::Error::INVALID_MONIKER);
  }

  // `realm_path` is a required vector of size >= 1.
  if (!appmgr_moniker.HasMember("realm_path") || !appmgr_moniker["realm_path"].IsArray() ||
      appmgr_moniker["realm_path"].GetArray().Size() < 1) {
    FX_LOGS(ERROR) << "appmgr_moniker.realm_path is a required, non-empty list.";
    return fit::error(ComponentIdIndex::Error::INVALID_MONIKER);
  }

  // `realm_path` elements must be strings.
  const auto& realm_path_json = appmgr_moniker["realm_path"].GetArray();
  std::vector<std::string> realm_path;
  for (const auto& realm_name : realm_path_json) {
    if (!realm_name.IsString()) {
      FX_LOGS(ERROR) << "appmgr_moniker.realm_path must be a list of strings.";
      return fit::error(ComponentIdIndex::Error::INVALID_MONIKER);
    }
    realm_path.push_back(realm_name.GetString());
  }

  return fit::ok(std::pair{
      Moniker{.url = appmgr_moniker["url"].GetString(), .realm_path = std::move(realm_path)},
      ComponentIdIndex::InstanceId(entry["instance_id"].GetString())});
}
}  // namespace

ComponentIdIndex::ComponentIdIndex(ComponentIdIndex::MonikerToInstanceId moniker_to_id)
    : moniker_to_id_(std::move(moniker_to_id)) {}

fit::result<ComponentIdIndex::MonikerToInstanceId, ComponentIdIndex::Error> Parse(
    const rapidjson::Document& doc) {
  if (!doc.IsObject()) {
    FX_LOGS(ERROR) << "Index must be a valid object.";
    return fit::error(ComponentIdIndex::Error::INVALID_SCHEMA);
  }

  // `instances` must be an array.
  if (!doc.HasMember("instances") || !doc["instances"].IsArray()) {
    FX_LOGS(ERROR) << "instances is a required list.";
    return fit::error(ComponentIdIndex::Error::INVALID_SCHEMA);
  }

  const auto& instances = doc["instances"].GetArray();
  ComponentIdIndex::MonikerToInstanceId moniker_to_id;
  std::unordered_set<ComponentIdIndex::InstanceId> instance_id_set;
  for (const auto& entry : instances) {
    auto parsed_entry = ParseEntry(entry);
    if (parsed_entry.is_error()) {
      return fit::error(parsed_entry.error());
    }

    auto id_result = instance_id_set.insert(parsed_entry.value().second);
    if (!id_result.second) {
      FX_LOGS(ERROR) << "The set of instance IDs must be unique.";
      // Instance ID already exists.
      return fit::error(ComponentIdIndex::Error::DUPLICATE_INSTANCE_ID);
    }

    auto result = moniker_to_id.insert(parsed_entry.take_value());
    if (!result.second) {
      FX_LOGS(ERROR) << "The set of appmgr_monikers must be unique.";
      // Moniker already exists in the map.
      return fit::error(ComponentIdIndex::Error::DUPLICATE_MONIKER);
    }
  }

  return fit::ok(moniker_to_id);
}

// static
fit::result<fbl::RefPtr<ComponentIdIndex>, ComponentIdIndex::Error>
ComponentIdIndex::CreateFromAppmgrConfigDir(const fxl::UniqueFD& appmgr_config_dir) {
  if (!files::IsFileAt(appmgr_config_dir.get(), kIndexFilePath)) {
    return fit::ok(fbl::AdoptRef(new ComponentIdIndex({})));
  }

  std::string file_contents;
  if (!files::ReadFileToStringAt(appmgr_config_dir.get(), kIndexFilePath, &file_contents)) {
    FX_LOGS(ERROR) << "Could not read instance ID index file.";
    return fit::error(Error::INVALID_JSON);
  }

  return CreateFromIndexContents(file_contents);
}

// static
fit::result<fbl::RefPtr<ComponentIdIndex>, ComponentIdIndex::Error>
ComponentIdIndex::CreateFromIndexContents(const std::string& index_contents) {
  rapidjson::Document doc;
  doc.Parse(index_contents.c_str());
  if (doc.HasParseError()) {
    FX_LOGS(ERROR) << "Could not json-parse instance ID index file.";
    return fit::error(Error::INVALID_JSON);
  }

  auto parse_retval = Parse(doc);
  if (parse_retval.is_error()) {
    return fit::error(parse_retval.take_error());
  }
  return fit::ok(fbl::AdoptRef(new ComponentIdIndex(parse_retval.take_value())));
}

std::optional<ComponentIdIndex::InstanceId> ComponentIdIndex::LookupMoniker(
    const Moniker& moniker) const {
  const auto& it = moniker_to_id_.find(moniker);
  if (it != moniker_to_id_.end()) {
    return it->second;
  }
  return std::nullopt;
}

}  // namespace component
