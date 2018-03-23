// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/ledger_client/storage.h"

#include <string>
#include <vector>

#include "lib/fxl/strings/join_strings.h"
#include "peridot/lib/util/string_escape.h"

namespace modular {

std::string MakeStoryKey(const f1dl::StringPtr& story_id) {
  // Not escaped, because only one component after the prefix.
  return kStoryKeyPrefix + story_id.get();
}

std::string MakeDeviceKey(const f1dl::StringPtr& device_name) {
  // Not escaped, because only one component after the prefix.
  return kDeviceKeyPrefix + device_name.get();
}

std::string MakePerDeviceKey(const f1dl::StringPtr& device_name) {
  // Not escaped, because only one component after the prefix.
  return kPerDeviceKeyPrefix + device_name.get();
}

std::string MakeFocusKey(const f1dl::StringPtr& device_name) {
  // Not escaped, because only one component after the prefix.
  return kFocusKeyPrefix + device_name.get();
}

std::string MakeMessageQueuesPrefix(const std::string& component_namespace) {
  std::string key{kMessageQueueTokenKeyPrefix};
  key.append(StringEscape(component_namespace, kSeparator, kEscaper));
  key.append(kSeparator);
  return key;
}

std::string MakeMessageQueueTokenKey(const std::string& component_namespace,
                                     const std::string& component_instance_id,
                                     const std::string& queue_name) {
  std::string key{kMessageQueueTokenKeyPrefix};
  key.append(StringEscape(component_namespace, kSeparator, kEscaper));
  key.append(kSeparator);
  key.append(StringEscape(component_instance_id, kSeparator, kEscaper));
  key.append(kSeparator);
  key.append(StringEscape(queue_name, kSeparator, kEscaper));
  return key;
}

std::string MakeMessageQueueKey(const std::string& queue_token) {
  // Not escaped, because only one component after the prefix.
  return kMessageQueueKeyPrefix + queue_token;
}

std::string EncodeModulePath(const f1dl::VectorPtr<f1dl::StringPtr>& module_path) {
  std::vector<std::string> segments;
  segments.reserve(module_path->size());
  for (const auto& module_path_part : *module_path) {
    segments.emplace_back(
        StringEscape(module_path_part.get(), kCharsToEscape, kEscaper));
  }
  return fxl::JoinStrings(segments, kSubSeparator);
}

std::string EncodeLinkPath(const LinkPathPtr& link_path) {
  std::string output;
  output.append(EncodeModulePath(link_path->module_path));
  output.append(kSeparator);
  output.append(
      StringEscape(link_path->link_name.get(), kCharsToEscape, kEscaper));
  return output;
}

std::string EncodeModuleComponentNamespace(const std::string& story_id) {
  // TODO(mesch): Needs escaping, and must not be escaped when used as component
  // of a full key. Messy.
  return "story:" + story_id;
}

std::string MakeTriggerKey(const std::string& agent_url,
                           const std::string& task_id) {
  std::string key{kTriggerKeyPrefix};
  key.append(StringEscape(agent_url, kCharsToEscape, kEscaper));
  key.append(kSeparator);
  key.append(StringEscape(task_id, kCharsToEscape, kEscaper));
  return key;
}

std::string MakeLinkKey(const LinkPathPtr& link_path) {
  std::string key{kLinkKeyPrefix};
  key.append(EncodeLinkPath(link_path));
  return key;
}

std::string MakeModuleKey(const f1dl::VectorPtr<f1dl::StringPtr>& module_path) {
  FXL_DCHECK(!module_path.is_null() && module_path->size() > 0)
      << EncodeModulePath(module_path);
  FXL_DCHECK(module_path->at(0)->size() > 0) << EncodeModulePath(module_path);
  std::string key{kModuleKeyPrefix};
  key.append(EncodeModulePath(module_path));
  return key;
}

}  // namespace modular
