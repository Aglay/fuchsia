// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SUGGESTION_ENGINE_RANKED_SUGGESTIONS_LIST_H_
#define PERIDOT_BIN_SUGGESTION_ENGINE_RANKED_SUGGESTIONS_LIST_H_

#include <functional>
#include <vector>

#include <fuchsia/cpp/modular.h>

#include "peridot/bin/suggestion_engine/ranker.h"
#include "peridot/bin/suggestion_engine/ranking_feature.h"
#include "peridot/bin/suggestion_engine/suggestion_prototype.h"

namespace modular {
using MatchPredicate =
    std::function<bool(const std::unique_ptr<RankedSuggestion>& suggestion)>;

// Stores a list of RankedSuggestion objects and the features by which they
// should be ranked.  Ranking must be explicitly triggered via the Rank
// method.
class RankedSuggestionsList {
 public:
  RankedSuggestionsList();
  ~RankedSuggestionsList();

  void SetRanker(std::unique_ptr<Ranker> ranker);

  void Rank(const UserInput& query = UserInput());

  void AddSuggestion(SuggestionPrototype* const prototype);

  // Returns |true| if and only if the suggestion was present and is removed.
  bool RemoveProposal(const std::string& component_url,
                      const std::string& proposal_id);

  void RemoveAllSuggestions();

  RankedSuggestion* GetSuggestion(const std::string& suggestion_id) const;

  RankedSuggestion* GetSuggestion(const std::string& component_url,
                                  const std::string& proposal_id) const;

  const std::vector<std::unique_ptr<RankedSuggestion>>& Get() const {
    return suggestions_;
  }

 private:
  RankedSuggestion* GetMatchingSuggestion(MatchPredicate matchFunction) const;
  bool RemoveMatchingSuggestion(MatchPredicate matchFunction);
  void DoStableSort();

  // The sorted vector of RankedSuggestions, sorted by
  // |ranking_function_|. The vector is re-sorted whenever its
  // contents are modified or when |ranking_function_| is updated.
  // TODO(jwnichols): Should ranking happen automatically or specifically
  // when requested?  I think I would lean toward the latter, since ranking
  // may be expensive.
  std::vector<std::unique_ptr<RankedSuggestion>> suggestions_;

  std::unique_ptr<Ranker> ranker_;
};

}  // namespace modular

#endif  // PERIDOT_BIN_SUGGESTION_ENGINE_RANKED_SUGGESTIONS_LIST_H_
