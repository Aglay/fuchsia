// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranking_features/proposal_hint_ranking_feature.h"

namespace modular {

ProposalHintRankingFeature::ProposalHintRankingFeature() = default;

ProposalHintRankingFeature::~ProposalHintRankingFeature() = default;

double ProposalHintRankingFeature::ComputeFeatureInternal(
    const fuchsia::modular::UserInput& query,
    const RankedSuggestion& suggestion) {
  return suggestion.prototype->proposal.confidence;
}

}  // namespace modular
