// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/process_config_loader.h"

#include <sstream>
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
static constexpr char kJsonKeyRate[] = "rate";
static constexpr char kJsonKeyEffect[] = "effect";
static constexpr char kJsonKeyConfig[] = "config";
static constexpr char kJsonKeyStreams[] = "streams";
static constexpr char kJsonKeyInputs[] = "inputs";
static constexpr char kJsonKeyEffects[] = "effects";
static constexpr char kJsonKeyLoopback[] = "loopback";
static constexpr char kJsonKeyDeviceId[] = "device_id";
static constexpr char kJsonKeyOutputRate[] = "output_rate";
static constexpr char kJsonKeyInputDevices[] = "input_devices";
static constexpr char kJsonKeyOutputDevices[] = "output_devices";
static constexpr char kJsonKeySupportedOutputStreamTypes[] = "supported_output_stream_types";
static constexpr char kJsonKeyEligibleForLoopback[] = "eligible_for_loopback";
static constexpr char kJsonKeyIndependentVolumeControl[] = "independent_volume_control";
static constexpr char kJsonKeyThermalPolicy[] = "thermal_policy";
static constexpr char kJsonKeyTargetName[] = "target_name";
static constexpr char kJsonKeyStates[] = "states";
static constexpr char kJsonKeyTripPoint[] = "trip_point";

void CountLoopbackStages(const PipelineConfig::MixGroup& mix_group, uint32_t* count) {
  if (mix_group.loopback) {
    ++*count;
  }
  for (const auto& input : mix_group.inputs) {
    CountLoopbackStages(input, count);
  }
}

uint32_t CountLoopbackStages(const PipelineConfig::MixGroup& root) {
  uint32_t count = 0;
  CountLoopbackStages(root, &count);
  return count;
}

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

std::optional<RenderUsage> RenderUsageFromString(std::string_view string) {
  if (string == "media" || string == "render:media") {
    return RenderUsage::MEDIA;
  } else if (string == "background" || string == "render:background") {
    return RenderUsage::BACKGROUND;
  } else if (string == "communications" || string == "render:communications") {
    return RenderUsage::COMMUNICATION;
  } else if (string == "interruption" || string == "render:interruption") {
    return RenderUsage::INTERRUPTION;
  } else if (string == "system_agent" || string == "render:system_agent") {
    return RenderUsage::SYSTEM_AGENT;
  } else if (string == "ultrasound" || string == "render:ultrasound") {
    return RenderUsage::ULTRASOUND;
  }
  return std::nullopt;
}

std::optional<CaptureUsage> CaptureUsageFromString(std::string_view string) {
  if (string == "background" || string == "capture:background") {
    return CaptureUsage::BACKGROUND;
  } else if (string == "foreground" || string == "capture:foreground") {
    return CaptureUsage::FOREGROUND;
  } else if (string == "system_agent" || string == "capture:system_agent") {
    return CaptureUsage::SYSTEM_AGENT;
  } else if (string == "communications" || string == "capture:communications") {
    return CaptureUsage::COMMUNICATION;
  } else if (string == "ultrasound" || string == "capture:ultrasound") {
    return CaptureUsage::ULTRASOUND;
  }
  return std::nullopt;
}

std::optional<StreamUsage> StreamUsageFromString(std::string_view string) {
  auto render_usage = RenderUsageFromString(string);
  if (render_usage) {
    return StreamUsage::WithRenderUsage(*render_usage);
  }
  auto capture_usage = CaptureUsageFromString(string);
  if (capture_usage) {
    return StreamUsage::WithCaptureUsage(*capture_usage);
  }
  return std::nullopt;
}

PipelineConfig::Effect ParseEffectFromJsonObject(const rapidjson::Value& value) {
  FX_CHECK(value.IsObject());
  PipelineConfig::Effect effect;

  auto it = value.FindMember(kJsonKeyLib);
  FX_CHECK(it != value.MemberEnd() && it->value.IsString());
  effect.lib_name = it->value.GetString();

  it = value.FindMember(kJsonKeyEffect);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsString());
    effect.effect_name = it->value.GetString();
  }

  it = value.FindMember(kJsonKeyName);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsString());
    effect.instance_name = it->value.GetString();
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
      auto render_usage = RenderUsageFromString(stream_type.GetString());
      FX_DCHECK(render_usage);
      mix_group.input_streams.push_back(*render_usage);
    }
  }

  it = value.FindMember(kJsonKeyEffects);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsArray());
    for (const auto& effect : it->value.GetArray()) {
      mix_group.effects.push_back(ParseEffectFromJsonObject(effect));
    }
  }

  it = value.FindMember(kJsonKeyInputs);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsArray());
    for (const auto& input : it->value.GetArray()) {
      mix_group.inputs.push_back(ParseMixGroupFromJsonObject(input));
    }
  }

  it = value.FindMember(kJsonKeyLoopback);
  if (it != value.MemberEnd()) {
    FX_CHECK(it->value.IsBool());
    mix_group.loopback = it->value.GetBool();
  } else {
    mix_group.loopback = false;
  }

  it = value.FindMember(kJsonKeyOutputRate);
  if (it != value.MemberEnd()) {
    FX_DCHECK(it->value.IsUint());
    mix_group.output_rate = it->value.GetUint();
  } else {
    mix_group.output_rate = PipelineConfig::kDefaultMixGroupRate;
  }
  return mix_group;
}

std::optional<audio_stream_unique_id_t> ParseDeviceIdFromJsonString(const rapidjson::Value& value) {
  FX_DCHECK(value.IsString());
  const auto* device_id_string = value.GetString();

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
    FX_DCHECK(captures == 16);
  }
  return device_id;
}

// Returns Some(vector) if there is a list of concrete device id's. Returns nullopt for the default
// configuration.
std::optional<std::vector<audio_stream_unique_id_t>> ParseDeviceIdFromJsonValue(
    const rapidjson::Value& value) {
  std::vector<audio_stream_unique_id_t> result;
  if (value.IsString()) {
    auto device_id = ParseDeviceIdFromJsonString(value);
    if (device_id) {
      result.push_back(*device_id);
    } else {
      return std::nullopt;
    }
  } else if (value.IsArray()) {
    for (const auto& device_id_value : value.GetArray()) {
      auto device_id = ParseDeviceIdFromJsonString(device_id_value);
      if (device_id) {
        result.push_back(*device_id);
      } else {
        return std::nullopt;
      }
    }
  }
  return {result};
}

std::pair<std::optional<std::vector<audio_stream_unique_id_t>>, DeviceConfig::OutputDeviceProfile>
ParseOutputDeviceProfileFromJsonObject(const rapidjson::Value& value,
                                       StreamUsageSet* all_supported_usages) {
  FX_CHECK(value.IsObject());

  auto device_id_it = value.FindMember(kJsonKeyDeviceId);
  FX_CHECK(device_id_it != value.MemberEnd());

  auto device_id = ParseDeviceIdFromJsonValue(device_id_it->value);

  auto eligible_for_loopback_it = value.FindMember(kJsonKeyEligibleForLoopback);
  FX_CHECK(eligible_for_loopback_it != value.MemberEnd());
  FX_CHECK(eligible_for_loopback_it->value.IsBool());
  const auto eligible_for_loopback = eligible_for_loopback_it->value.GetBool();

  auto independent_volume_control_it = value.FindMember(kJsonKeyIndependentVolumeControl);
  bool independent_volume_control = false;
  if (independent_volume_control_it != value.MemberEnd()) {
    FX_CHECK(independent_volume_control_it->value.IsBool());
    independent_volume_control = independent_volume_control_it->value.GetBool();
  }

  auto supported_stream_types_it = value.FindMember(kJsonKeySupportedOutputStreamTypes);
  FX_CHECK(supported_stream_types_it != value.MemberEnd());
  auto& supported_stream_types_value = supported_stream_types_it->value;
  FX_CHECK(supported_stream_types_value.IsArray());

  StreamUsageSet supported_stream_types;
  for (const auto& stream_type : supported_stream_types_value.GetArray()) {
    FX_CHECK(stream_type.IsString());
    const auto supported_usage = StreamUsageFromString(stream_type.GetString());
    FX_DCHECK(supported_usage);
    all_supported_usages->insert(*supported_usage);
    supported_stream_types.insert(*supported_usage);
  }

  auto pipeline_it = value.FindMember(kJsonKeyPipeline);
  PipelineConfig pipeline_config;
  if (pipeline_it != value.MemberEnd()) {
    FX_CHECK(pipeline_it->value.IsObject());
    auto root = ParseMixGroupFromJsonObject(pipeline_it->value);
    auto loopback_stages = CountLoopbackStages(root);
    FX_CHECK(loopback_stages <= 1);
    if (loopback_stages == 0) {
      root.loopback = true;
    }
    pipeline_config = PipelineConfig(std::move(root));
  } else {
    pipeline_config = PipelineConfig::Default();
  }

  return {device_id, DeviceConfig::OutputDeviceProfile(
                         eligible_for_loopback, std::move(supported_stream_types),
                         independent_volume_control, std::move(pipeline_config))};
}

ThermalConfig::Entry ParseThermalPolicyEntryFromJsonObject(const rapidjson::Value& value) {
  FX_CHECK(value.IsObject());

  auto target_name_it = value.FindMember(kJsonKeyTargetName);
  FX_CHECK(target_name_it != value.MemberEnd());
  FX_CHECK(target_name_it->value.IsString());
  const auto* target_name = target_name_it->value.GetString();

  auto states_it = value.FindMember(kJsonKeyStates);
  FX_CHECK(states_it != value.MemberEnd());
  FX_CHECK(states_it->value.IsArray());
  auto states_array = states_it->value.GetArray();

  std::vector<ThermalConfig::State> states;
  states.reserve(states_array.Size());

  for (const auto& state : states_array) {
    FX_CHECK(state.IsObject());

    auto trip_point_it = state.FindMember(kJsonKeyTripPoint);
    FX_CHECK(trip_point_it != state.MemberEnd());
    FX_CHECK(trip_point_it->value.IsUint());
    FX_CHECK(trip_point_it->value.GetUint() >= 1);
    FX_CHECK(trip_point_it->value.GetUint() <= 100);
    auto trip_point = trip_point_it->value.GetUint();

    auto config_it = state.FindMember(kJsonKeyConfig);
    if (config_it != state.MemberEnd()) {
      rapidjson::StringBuffer config_buf;
      rapidjson::Writer<rapidjson::StringBuffer> writer(config_buf);
      config_it->value.Accept(writer);
      states.emplace_back(trip_point, config_buf.GetString());
    } else {
      states.emplace_back(trip_point, "");
    }
  }

  return ThermalConfig::Entry(target_name, states);
}

void ParseOutputDevicePoliciesFromJsonObject(const rapidjson::Value& output_device_profiles,
                                             ProcessConfigBuilder* config_builder) {
  FX_CHECK(output_device_profiles.IsArray());

  StreamUsageSet all_supported_usages;
  for (const auto& output_device_profile : output_device_profiles.GetArray()) {
    config_builder->AddDeviceProfile(
        ParseOutputDeviceProfileFromJsonObject(output_device_profile, &all_supported_usages));
  }

  // We expect all the usages that clients can select are supported.
  for (const auto& render_usage : kFidlRenderUsages) {
    FX_CHECK(all_supported_usages.find(StreamUsage::WithRenderUsage(render_usage)) !=
             all_supported_usages.end());
  }
  // Not all devices will support ultrasound.
  if (all_supported_usages.find(StreamUsage::WithRenderUsage(RenderUsage::ULTRASOUND)) ==
      all_supported_usages.end()) {
    FX_LOGS(INFO) << "Device does not support ultrasound";
  }
}

std::pair<std::optional<std::vector<audio_stream_unique_id_t>>, DeviceConfig::InputDeviceProfile>
ParseInputDeviceProfileFromJsonObject(const rapidjson::Value& value) {
  FX_CHECK(value.IsObject());

  auto device_id_it = value.FindMember(kJsonKeyDeviceId);
  FX_CHECK(device_id_it != value.MemberEnd());

  auto device_id = ParseDeviceIdFromJsonValue(device_id_it->value);

  auto rate_it = value.FindMember(kJsonKeyRate);
  FX_DCHECK(rate_it != value.MemberEnd());
  FX_DCHECK(rate_it->value.IsUint());
  auto rate = rate_it->value.GetUint();

  return {device_id, DeviceConfig::InputDeviceProfile(rate)};
}

void ParseInputDevicePoliciesFromJsonObject(const rapidjson::Value& input_device_profiles,
                                            ProcessConfigBuilder* config_builder) {
  FX_CHECK(input_device_profiles.IsArray());

  for (const auto& input_device_profile : input_device_profiles.GetArray()) {
    config_builder->AddDeviceProfile(ParseInputDeviceProfileFromJsonObject(input_device_profile));
  }
}

void ParseThermalPolicyFromJsonObject(const rapidjson::Value& value,
                                      ProcessConfigBuilder* config_builder) {
  FX_CHECK(value.IsArray());

  for (const auto& thermal_policy_entry : value.GetArray()) {
    config_builder->AddThermalPolicyEntry(
        ParseThermalPolicyEntryFromJsonObject(thermal_policy_entry));
  }
}

}  // namespace

std::optional<ProcessConfig> ProcessConfigLoader::LoadProcessConfig(const char* filename) {
  std::string buffer;
  const auto file_exists = files::ReadFileToString(filename, &buffer);
  if (!file_exists) {
    return std::nullopt;
  }

  auto result = ParseProcessConfig(buffer);
  if (result.is_error()) {
    FX_LOGS(FATAL) << "Failed to parse " << filename << "; error: " << result.error();
  }

  return result.take_value();
}

fit::result<ProcessConfig, std::string> ProcessConfigLoader::ParseProcessConfig(
    const std::string& config) {
  rapidjson::Document doc;
  std::string parse_buffer = config;
  const rapidjson::ParseResult parse_res = doc.ParseInsitu(parse_buffer.data());
  if (parse_res.IsError()) {
    std::stringstream error;
    error << "Parse error (" << rapidjson::GetParseError_En(parse_res.Code())
          << "): " << parse_res.Offset();
    return fit::error(error.str());
  }

  const auto schema = LoadProcessConfigSchema();
  rapidjson::SchemaValidator validator(schema);
  if (!doc.Accept(validator)) {
    rapidjson::StringBuffer error_buf;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(error_buf);
    validator.GetError().Accept(writer);
    std::stringstream error;
    error << "Schema validation error (" << error_buf.GetString() << ")";
    return fit::error(error.str());
  }

  auto curve_result = ParseVolumeCurveFromJsonObject(doc[kJsonKeyVolumeCurve]);
  if (!curve_result.is_ok()) {
    std::stringstream error;
    error << "Invalid volume curve; error: " << curve_result.take_error();
    return fit::error(error.str());
  }

  auto config_builder = ProcessConfig::Builder();
  config_builder.SetDefaultVolumeCurve(curve_result.take_value());

  auto output_devices_it = doc.FindMember(kJsonKeyOutputDevices);
  if (output_devices_it != doc.MemberEnd()) {
    ParseOutputDevicePoliciesFromJsonObject(output_devices_it->value, &config_builder);
  }
  auto input_devices_it = doc.FindMember(kJsonKeyInputDevices);
  if (input_devices_it != doc.MemberEnd()) {
    ParseInputDevicePoliciesFromJsonObject(input_devices_it->value, &config_builder);
  }

  auto thermal_policy_it = doc.FindMember(kJsonKeyThermalPolicy);
  if (thermal_policy_it != doc.MemberEnd()) {
    ParseThermalPolicyFromJsonObject(thermal_policy_it->value, &config_builder);
  }

  return fit::ok(config_builder.Build());
}

}  // namespace media::audio
