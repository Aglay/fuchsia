// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SUGGESTION_ENGINE_RANKER_H_
#define PERIDOT_BIN_SUGGESTION_ENGINE_RANKER_H_

#include <fuchsia/cpp/modular.h>

#include "peridot/bin/suggestion_engine/ranked_suggestion.h"

namespace modular {

// Base class for performing ranking on a suggestion.
class Ranker {
 public:
  virtual ~Ranker();

  // Ranks a suggestion based on a given query (which might be empty) and
  // returns the confidence that would be the new suggestion.confidence.
  virtual double Rank(
      const UserInput& query, const RankedSuggestion& suggestion) = 0;
};

}  // namespace modular

#endif  // PERIDOT_BIN_SUGGESTION_ENGINE_RANKER_H_
