// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/process_config_loader.h"

#include <string_view>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/schema.h>

#include "rapidjson/prettywriter.h"
#include "src/lib/files/file.h"
#include "src/lib/syslog/cpp/logger.h"

#include "src/media/audio/audio_core/schema/audio_core_config_schema.inl"

namespace media::audio {

namespace {

static constexpr char kJsonKeyVolumeCurve[] = "volume_curve";
static constexpr char kJsonKeyPipeline[] = "pipeline";
static constexpr char kJsonKeyLib[] = "lib";
static constexpr char kJsonKeyName[] = "name";
static constexpr char kJsonKeyConfig[] = "config";
static constexpr char kJsonKeyStreams[] = "streams";
static constexpr char kJsonKeyEffects[] = "effects";
static constexpr char kJsonKeyOutputStreams[] = "output_streams";
static constexpr char kJsonKeyMix[] = "mix";
static constexpr char kJsonKeyLinearize[] = "linearize";
static constexpr char kJsonKeyRoutingPolicy[] = "routing_policy";
static constexpr char kJsonKeyDeviceProfiles[] = "device_profiles";
static constexpr char kJsonKeyDeviceId[] = "device_id";
static constexpr char kJsonKeySupportedOutputStreamTypes[] = "supported_output_stream_types";

rapidjson::SchemaDocument LoadProcessConfigSchema() {
  rapidjson::Document schema_doc;
  const rapidjson::ParseResult result = schema_doc.Parse(kAudioCoreConfigSchema);
  FX_CHECK(!result.IsError()) << rapidjson::GetParseError_En(result.Code()) << "("
                              << result.Offset() << ")";
  return rapidjson::SchemaDocument(schema_doc);
}

fit::result<VolumeCurve, VolumeCurve::Error> ParseVolumeCurveFromJsonObject(
    const rapidjson::Value& value) {
  FX_CHECK(value.IsArray());
  std::vector<VolumeCurve::VolumeMapping> mappings;
  for (const auto& mapping : value.GetArray()) {
    mappings.emplace_back(mapping["level"].GetFloat(), mapping["db"].GetFloat());
  }

  return VolumeCurve::FromMappings(std::move(mappings));
}

fuchsia::media::AudioRenderUsage UsageFromString(std::string_view string) {
  if (string == "media") {
    return fuchsia::media::AudioRenderUsage::MEDIA;
  } else if (string == "background") {
    return fuchsia::media::AudioRenderUsage::BACKGROUND;
  } else if (string == "communications") {
    return fuchsia::media::AudioRenderUsage::COMMUNICATION;
  } else if (string == "interruption") {
    return fuchsia::media::AudioRenderUsage::INTERRUPTION;
  } else if (string == "system_agent") {
    return fuchsia::media::AudioRenderUsage::SYSTEM_AGENT;
  }
  FX_CHECK(false);
  return fuchsia::media::AudioRenderUsage::MEDIA;
}

PipelineConfig::Effect ParseEffectFromJsonObject(const rapidjson::Value& value) {
  FX_CHECK(value.IsObject());
  PipelineConfig::Effect effect;

  auto it = value.FindMember(kJsonKeyLib);
  FX_CHECK(it != value.MemberEnd() && it->value.IsString());
  effect.lib_name = it->value.GetString();

  it = value.FindMember(kJsonKeyName);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsString());
    effect.effect_name = it->value.GetString();
  }

  it = value.FindMember(kJsonKeyConfig);
  if (it != value.MemberEnd()) {
    rapidjson::StringBuffer config_buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(config_buf);
    it->value.Accept(writer);
    effect.effect_config = config_buf.GetString();
  }
  return effect;
}

PipelineConfig::MixGroup ParseMixGroupFromJsonObject(const rapidjson::Value& value) {
  FX_CHECK(value.IsObject());
  PipelineConfig::MixGroup mix_group;

  auto it = value.FindMember(kJsonKeyName);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsString());
    mix_group.name = it->value.GetString();
  }

  it = value.FindMember(kJsonKeyStreams);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsArray());
    for (const auto& stream_type : it->value.GetArray()) {
      FX_CHECK(stream_type.IsString());
      mix_group.input_streams.push_back(UsageFromString(stream_type.GetString()));
    }
  }

  it = value.FindMember(kJsonKeyEffects);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsArray());
    for (const auto& effect : it->value.GetArray()) {
      mix_group.effects.push_back(ParseEffectFromJsonObject(effect));
    }
  }
  return mix_group;
}

void ParsePipelineConfigFromJsonObject(const rapidjson::Value& value,
                                       ProcessConfig::Builder* config_builder) {
  FXL_CHECK(value.IsObject());

  auto it = value.FindMember(kJsonKeyOutputStreams);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsArray());
    for (const auto& group : it->value.GetArray()) {
      config_builder->AddOutputStreamEffects(ParseMixGroupFromJsonObject(group));
    }
  }

  it = value.FindMember(kJsonKeyMix);
  if (it != value.MemberEnd()) {
    config_builder->SetMixEffects(ParseMixGroupFromJsonObject(it->value));
  }

  it = value.FindMember(kJsonKeyLinearize);
  if (it != value.MemberEnd()) {
    config_builder->SetLinearizeEffects(ParseMixGroupFromJsonObject(it->value));
  }
}

std::pair<std::optional<audio_stream_unique_id_t>, RoutingConfig::UsageSupportSet>
ParseDeviceRoutingProfileFromJsonObject(const rapidjson::Value& value,
                                        std::unordered_set<uint32_t>* all_supported_usages) {
  FXL_CHECK(value.IsObject());

  auto device_id_it = value.FindMember(kJsonKeyDeviceId);
  FXL_CHECK(device_id_it != value.MemberEnd());
  auto& device_id_value = device_id_it->value;
  FXL_CHECK(device_id_value.IsString());
  const auto* device_id_string = device_id_value.GetString();

  std::optional<audio_stream_unique_id_t> device_id;
  if (strcmp(device_id_string, "*") != 0) {
    device_id = {{}};
    const auto captures = std::sscanf(
        device_id_string,
        "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8
        "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8 "%02" SCNx8
        "%02" SCNx8 "%02" SCNx8,
        &device_id->data[0], &device_id->data[1], &device_id->data[2], &device_id->data[3],
        &device_id->data[4], &device_id->data[5], &device_id->data[6], &device_id->data[7],
        &device_id->data[8], &device_id->data[9], &device_id->data[10], &device_id->data[11],
        &device_id->data[12], &device_id->data[13], &device_id->data[14], &device_id->data[15]);
    FXL_CHECK(captures == 16);
  }

  auto supported_output_stream_types_it = value.FindMember(kJsonKeySupportedOutputStreamTypes);
  FXL_CHECK(supported_output_stream_types_it != value.MemberEnd());
  auto& supported_output_stream_types_value = supported_output_stream_types_it->value;
  FXL_CHECK(supported_output_stream_types_value.IsArray());

  RoutingConfig::UsageSupportSet supported_output_stream_types;
  for (const auto& stream_type : supported_output_stream_types_value.GetArray()) {
    FXL_CHECK(stream_type.IsString());
    const auto supported_usage = fidl::ToUnderlying(UsageFromString(stream_type.GetString()));
    all_supported_usages->insert(supported_usage);
    supported_output_stream_types.insert(supported_usage);
  }

  return {device_id, std::move(supported_output_stream_types)};
}

void ParseRoutingPolicyFromJsonObject(const rapidjson::Value& value,
                                      ProcessConfigBuilder* config_builder) {
  FXL_CHECK(value.IsObject());

  auto device_profiles_it = value.FindMember(kJsonKeyDeviceProfiles);
  FXL_CHECK(device_profiles_it != value.MemberEnd());
  auto& device_profiles = device_profiles_it->value;
  FXL_CHECK(device_profiles.IsArray());

  std::unordered_set<uint32_t> all_supported_usages;
  for (const auto& device_profile : device_profiles.GetArray()) {
    config_builder->AddDeviceRoutingProfile(
        ParseDeviceRoutingProfileFromJsonObject(device_profile, &all_supported_usages));
  }

  FXL_CHECK(all_supported_usages.size() == fuchsia::media::RENDER_USAGE_COUNT)
      << "Not all output usages are supported in the config";
}

}  // namespace

std::optional<ProcessConfig> ProcessConfigLoader::LoadProcessConfig(const char* filename) {
  std::string buffer;
  const auto file_exists = files::ReadFileToString(filename, &buffer);
  if (!file_exists) {
    return std::nullopt;
  }

  rapidjson::Document doc;
  const rapidjson::ParseResult parse_res = doc.ParseInsitu(buffer.data());
  if (parse_res.IsError()) {
    FX_LOGS(FATAL) << "Parse error (" << rapidjson::GetParseError_En(parse_res.Code())
                   << ") when reading " << filename << ":" << parse_res.Offset();
  }

  const auto schema = LoadProcessConfigSchema();
  rapidjson::SchemaValidator validator(schema);
  if (!doc.Accept(validator)) {
    rapidjson::StringBuffer error_buf;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(error_buf);
    validator.GetError().Accept(writer);
    FX_LOGS(FATAL) << "Schema validation error (" << error_buf.GetString() << ") when reading "
                   << filename;
  }

  auto curve_result = ParseVolumeCurveFromJsonObject(doc[kJsonKeyVolumeCurve]);
  if (!curve_result.is_ok()) {
    FX_LOGS(FATAL) << "Invalid volume curve; error: " << curve_result.take_error();
  }

  auto config_builder = ProcessConfig::Builder();
  config_builder.SetDefaultVolumeCurve(curve_result.take_value());

  // Add in audio effects if any are present.
  auto it = doc.FindMember(kJsonKeyPipeline);
  if (it != doc.MemberEnd()) {
    ParsePipelineConfigFromJsonObject(it->value, &config_builder);
  }

  auto routing_policy_it = doc.FindMember(kJsonKeyRoutingPolicy);
  if (routing_policy_it != doc.MemberEnd()) {
    ParseRoutingPolicyFromJsonObject(routing_policy_it->value, &config_builder);
  }

  return {config_builder.Build()};
}

}  // namespace media::audio
