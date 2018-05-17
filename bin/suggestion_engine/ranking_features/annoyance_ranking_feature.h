// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURES_ANNOYANCE_RANKING_FEATURE_H_
#define PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURES_ANNOYANCE_RANKING_FEATURE_H_

#include <modular/cpp/fidl.h>

#include "peridot/bin/suggestion_engine/ranking_feature.h"

namespace modular {

class AnnoyanceRankingFeature : public RankingFeature {
 public:
  AnnoyanceRankingFeature();
  ~AnnoyanceRankingFeature() override;

 private:
  double ComputeFeatureInternal(
      const UserInput& query, const RankedSuggestion& suggestion) override;
};

}  // namespace modular

#endif  // PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURES_ANNOYANCE_RANKING_FEATURE_H_
