// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fxl/files/file.h"
#include "lib/fxl/logging.h"
#include "peridot/bin/suggestion_engine/ranking_feature.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/schema.h"
#include "rapidjson/stringbuffer.h"
#include "third_party/rapidjson/rapidjson/document.h"

namespace maxwell {

int RankingFeature::instances_ = 0;

RankingFeature::RankingFeature() : id_(instances_++) {};

RankingFeature::~RankingFeature() = default;

double RankingFeature::ComputeFeature(
    const UserInput& query, const RankedSuggestion& suggestion,
    const f1dl::VectorPtr<ContextValuePtr>& context_update_values) {
  const double feature = ComputeFeatureInternal(
      query, suggestion, context_update_values);
  FXL_CHECK(feature <= kMaxConfidence);
  FXL_CHECK(feature >= kMinConfidence);
  return feature;
}

ContextSelectorPtr RankingFeature::CreateContextSelector() {
  return CreateContextSelectorInternal();
}

const std::string RankingFeature::UniqueId() const {
  std::stringstream ss;
  ss << "rf_" << id_;
  return ss.str();
}

ContextSelectorPtr RankingFeature::CreateContextSelectorInternal() {
  // By default we return a nullptr, meaning that the ranking feature doesn't
  // require context. If a ranking feature requires context, it should create a
  // context selector, set the values it needs and return it.
  return nullptr;
}

std::pair<bool, rapidjson::Document> RankingFeature::FetchJsonObject(
    const std::string& path) {
  // Load data file to string.
  std::string data;
  rapidjson::Document data_doc;
  if (!files::ReadFileToString(path, &data)) {
    FXL_LOG(WARNING) << "Missing ranking feature data file: " << path;
    return std::make_pair(false, std::move(data_doc));
  }

  // Parse json data string.
  data_doc.Parse(data);
  if (data_doc.HasParseError()) {
    std::string error;
    FXL_LOG(WARNING) << "Invalid JSON (" << path << " at "
                     << data_doc.GetErrorOffset() << "): "
                     << rapidjson::GetParseError_En(data_doc.GetParseError());
    return std::make_pair(false, std::move(data_doc));
  }

  return std::make_pair(true, std::move(data_doc));
}

}  // namespace maxwell
