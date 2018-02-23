// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/tests/maxwell_integration/test_suggestion_listener.h"

#include "lib/suggestion/cpp/formatting.h"

bool suggestion_less(const maxwell::Suggestion* a,
                     const maxwell::Suggestion* b) {
  return a->confidence > b->confidence;
}

void TestSuggestionListener::OnInterrupt(maxwell::SuggestionPtr suggestion) {
  FXL_LOG(INFO) << "OnInterrupt(" << suggestion->uuid << ")";

  ClearSuggestions();

  auto insert_head = ordered_suggestions_.begin();
  insert_head = std::upper_bound(insert_head, ordered_suggestions_.end(),
                                 suggestion.get(), suggestion_less);
  insert_head = ordered_suggestions_.emplace(insert_head, suggestion.get()) + 1;
  suggestions_by_id_[suggestion->uuid] = std::move(suggestion);

  EXPECT_EQ((signed)ordered_suggestions_.size(),
            (signed)suggestions_by_id_.size());
}

void TestSuggestionListener::OnNextResults(
    f1dl::Array<maxwell::SuggestionPtr> suggestions) {
  FXL_LOG(INFO) << "OnNextResults(" << suggestions << ")";

  OnAnyResults(suggestions);
}

void TestSuggestionListener::OnQueryResults(
    f1dl::Array<maxwell::SuggestionPtr> suggestions) {
  FXL_LOG(INFO) << "OnQueryResults(" << suggestions << ")";

  OnAnyResults(suggestions);
}

void TestSuggestionListener::OnAnyResults(
    f1dl::Array<maxwell::SuggestionPtr>& suggestions) {
  ClearSuggestions();

  auto insert_head = ordered_suggestions_.begin();
  for (auto& suggestion : suggestions) {
    insert_head = std::upper_bound(insert_head, ordered_suggestions_.end(),
                                   suggestion.get(), suggestion_less);
    insert_head =
        ordered_suggestions_.emplace(insert_head, suggestion.get()) + 1;
    suggestions_by_id_[suggestion->uuid] = std::move(suggestion);
  }

  EXPECT_EQ((signed)ordered_suggestions_.size(),
            (signed)suggestions_by_id_.size());
}

void TestSuggestionListener::ClearSuggestions() {
  // For use when the listener_binding_ is reset
  ordered_suggestions_.clear();
  suggestions_by_id_.clear();
}

void TestSuggestionListener::OnProcessingChange(bool processing) {
  FXL_LOG(INFO) << "OnProcessingChange to " << processing;
}

void TestSuggestionListener::OnQueryComplete() {
  FXL_LOG(INFO) << "OnQueryComplete";
}
