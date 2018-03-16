// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranked_suggestions_list.h"

#include <algorithm>
#include <string>

#include "lib/context/cpp/context_helper.h"
#include "lib/fxl/logging.h"

namespace maxwell {

MatchPredicate GetSuggestionMatcher(const std::string& component_url,
                                    const std::string& proposal_id) {
  return [component_url,
          proposal_id](const std::unique_ptr<RankedSuggestion>& suggestion) {
    return (suggestion->prototype->proposal->id == proposal_id) &&
           (suggestion->prototype->source_url == component_url);
  };
}

MatchPredicate GetSuggestionMatcher(const std::string& suggestion_id) {
  return [suggestion_id](const std::unique_ptr<RankedSuggestion>& suggestion) {
    return suggestion->prototype->suggestion_id == suggestion_id;
  };
}

RankedSuggestionsList::RankedSuggestionsList() : normalization_factor_(0.0) {}

RankedSuggestionsList::~RankedSuggestionsList() = default;

RankedSuggestion* RankedSuggestionsList::GetMatchingSuggestion(
    MatchPredicate matchFunction) const {
  auto findIter =
      std::find_if(suggestions_.begin(), suggestions_.end(), matchFunction);
  if (findIter != suggestions_.end())
    return findIter->get();
  return nullptr;
}

bool RankedSuggestionsList::RemoveMatchingSuggestion(
    MatchPredicate matchFunction) {
  auto remove_iter =
      std::remove_if(suggestions_.begin(), suggestions_.end(), matchFunction);
  if (remove_iter == suggestions_.end()) {
    return false;
  } else {
    suggestions_.erase(remove_iter, suggestions_.end());
    return true;
  }
}

void RankedSuggestionsList::AddRankingFeature(
    double weight,
    std::shared_ptr<RankingFeature> ranking_feature) {
  ranking_features_.emplace_back(weight, ranking_feature);
  // only incorporate positive weights into the normalization factor
  if (weight > 0.0)
    normalization_factor_ += weight;
}

void RankedSuggestionsList::Rank(
    const UserInput& query, const ContextUpdatePtr& context_update) {
  for (auto& suggestion : suggestions_) {
    double confidence = 0.0;
    for (auto& feature : ranking_features_) {
      f1dl::Array<ContextValuePtr> context_values = TakeContextValue(
          context_update.get(), feature.second->UniqueId()).second;
      confidence +=
          feature.first *
          feature.second->ComputeFeature(query, *suggestion, context_values);
    }
    // TODO(jwnichols): Reconsider this normalization approach.
    // Weights may be negative, so there is some chance that the calculated
    // confidence score will be negative.  We pull the calculated score up to
    // zero to guarantee final confidence values stay within the 0-1 range.
    FXL_CHECK(normalization_factor_ > 0.0);
    suggestion->confidence = std::max(confidence, 0.0) / normalization_factor_;
    FXL_VLOG(1) << "Proposal "
                << suggestion->prototype->proposal->display->headline
                << " confidence " << suggestion->prototype->proposal->confidence
                << " => " << suggestion->confidence;
  }
  DoStableSort();
}

void RankedSuggestionsList::AddSuggestion(SuggestionPrototype* prototype) {
  std::unique_ptr<RankedSuggestion> ranked_suggestion =
      std::make_unique<RankedSuggestion>();
  ranked_suggestion->prototype = prototype;
  suggestions_.push_back(std::move(ranked_suggestion));
}

bool RankedSuggestionsList::RemoveProposal(const std::string& component_url,
                                           const std::string& proposal_id) {
  return RemoveMatchingSuggestion(
      GetSuggestionMatcher(component_url, proposal_id));
}

RankedSuggestion* RankedSuggestionsList::GetSuggestion(
    const std::string& suggestion_id) const {
  return GetMatchingSuggestion(GetSuggestionMatcher(suggestion_id));
}

RankedSuggestion* RankedSuggestionsList::GetSuggestion(
    const std::string& component_url,
    const std::string& proposal_id) const {
  return GetMatchingSuggestion(
      GetSuggestionMatcher(component_url, proposal_id));
}

void RankedSuggestionsList::RemoveAllSuggestions() {
  suggestions_.clear();
}

// Start of private sorting methods.

void RankedSuggestionsList::DoStableSort() {
  std::stable_sort(suggestions_.begin(), suggestions_.end(),
                   [](const std::unique_ptr<RankedSuggestion>& a,
                      const std::unique_ptr<RankedSuggestion>& b) {
                     return a->confidence > b->confidence;
                   });
}

// End of private sorting methods.

}  // namespace maxwell
