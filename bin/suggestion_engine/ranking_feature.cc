// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranking_feature.h"

#include "lib/fxl/logging.h"

namespace maxwell {

RankingFeature::RankingFeature() = default;

RankingFeature::~RankingFeature() = default;

double RankingFeature::ComputeFeature(const UserInput& query,
                                      const RankedSuggestion& suggestion,
                                      const ContextUpdatePtr& context_update) {
  const double feature = ComputeFeatureInternal(
      query, suggestion, context_update);
  FXL_CHECK(feature <= kMaxConfidence);
  FXL_CHECK(feature >= kMinConfidence);
  return feature;
}

}  // namespace maxwell
