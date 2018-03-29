// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranked_suggestion.h"

namespace modular {

Suggestion CreateSuggestion(const RankedSuggestion& suggestion_data) {
  Suggestion suggestion = CreateSuggestion(*suggestion_data.prototype);
  suggestion.confidence = suggestion_data.confidence;
  return suggestion;
}

}  // namespace modular
