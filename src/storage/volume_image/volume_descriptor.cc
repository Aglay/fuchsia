// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/volume_image/volume_descriptor.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "src/storage/volume_image/options.h"
#include "src/storage/volume_image/utils/guid.h"

namespace storage::volume_image {

fit::result<VolumeDescriptor, std::string> VolumeDescriptor::Deserialize(
    fbl::Span<const uint8_t> serialized) {
  rapidjson::Document document;
  rapidjson::ParseResult result =
      document.Parse(reinterpret_cast<const char*>(serialized.data()), serialized.size());

  if (result.IsError()) {
    std::ostringstream error;
    error << "Error parsing serialized VolumeDescriptor. "
          << rapidjson::GetParseError_En(result.Code()) << std::endl;
    return fit::error(error.str());
  }

  uint64_t magic = document["magic"].GetUint64();
  if (magic != kMagic) {
    return fit::error("Invalid Magic\n");
  }

  VolumeDescriptor descriptor = {};
  const std::string& instance_guid = document["instance_guid"].GetString();
  // The stringified version includes 4 Hyphens.
  if (instance_guid.length() != kGuidStrLength) {
    return fit::error("instance_guid length must be 36 bytes.\n");
  }
  auto instance_bytes = Guid::FromString(instance_guid);
  if (instance_bytes.is_error()) {
    return instance_bytes.take_error_result();
  }
  descriptor.instance = instance_bytes.take_value();

  const std::string& type_guid = document["type_guid"].GetString();
  // The stringified version includes 4 Hyphens.
  if (type_guid.length() != kGuidStrLength) {
    return fit::error("type_guid length must be 36 bytes.\n");
  }

  auto type_bytes = Guid::FromString(type_guid);
  if (type_bytes.is_error()) {
    return type_bytes.take_error_result();
  }
  descriptor.type = type_bytes.take_value();

  const std::string& name = document["name"].GetString();
  memcpy(descriptor.name.data(), name.c_str(), name.length());

  descriptor.block_size = document["block_size"].GetUint64();
  auto& compression = descriptor.compression;
  auto compression_enum =
      StringAsEnum<CompressionSchema>(document["compression_schema"].GetString());
  if (compression_enum.is_error()) {
    return compression_enum.take_error_result();
  }
  compression.schema = compression_enum.take_value();

  if (document.HasMember("compression_options")) {
    const auto& option_map = document["compression_options"].GetObject();
    for (auto& option : option_map) {
      compression.options[option.name.GetString()] = option.value.GetUint64();
    }
  }
  auto encryption_enum = StringAsEnum<EncryptionType>(document["encryption_type"].GetString());
  if (encryption_enum.is_error()) {
    return encryption_enum.take_error_result();
  }
  descriptor.encryption = encryption_enum.take_value();

  if (document.HasMember("options")) {
    const auto& option_set = document["options"].GetArray();
    for (auto& option : option_set) {
      auto option_enum = StringAsEnum<Option>(option.GetString());
      if (option_enum.is_error()) {
        return option_enum.take_error_result();
      }
      descriptor.options.insert(option_enum.take_value());
    }
  }

  return fit::ok(descriptor);
}

fit::result<std::vector<uint8_t>, std::string> VolumeDescriptor::Serialize() const {
  rapidjson::Document document;
  document.SetObject();

  document.AddMember("magic", kMagic, document.GetAllocator());
  auto instance_str = Guid::ToString(instance);
  if (instance_str.is_error()) {
    return instance_str.take_error_result();
  }
  document.AddMember("instance_guid", instance_str.take_value(), document.GetAllocator());

  auto type_str = Guid::ToString(type);
  if (type_str.is_error()) {
    return type_str.take_error_result();
  }
  document.AddMember("type_guid", type_str.take_value(), document.GetAllocator());
  document.AddMember("name", std::string(reinterpret_cast<const char*>(name.data())),
                     document.GetAllocator());
  document.AddMember("block_size", block_size, document.GetAllocator());
  document.AddMember("encryption_type", EnumAsString(encryption), document.GetAllocator());
  document.AddMember("compression_schema", EnumAsString(compression.schema),
                     document.GetAllocator());

  if (!compression.options.empty()) {
    rapidjson::Value option_map;
    option_map.SetObject();
    for (const auto& option : compression.options) {
      rapidjson::Value key(option.first.c_str(), document.GetAllocator());
      rapidjson::Value value(option.second);
      option_map.AddMember(key, value, document.GetAllocator());
    }
    document.AddMember("compression_options", option_map, document.GetAllocator());
  }

  if (!options.empty()) {
    rapidjson::Value option_set;
    option_set.SetArray();
    for (const auto& option : options) {
      rapidjson::Value value(EnumAsString(option).c_str(), document.GetAllocator());
      option_set.PushBack(value, document.GetAllocator());
    }
    document.AddMember("options", option_set, document.GetAllocator());
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  if (!document.Accept(writer)) {
    return fit::error("Failed to obtain string representation of VolumeDescriptor.\n");
  }

  const auto* serialized_content = reinterpret_cast<const uint8_t*>(buffer.GetString());
  std::vector<uint8_t> data(serialized_content, serialized_content + buffer.GetLength());
  data.push_back('\0');

  return fit::ok(data);
}

}  // namespace storage::volume_image
