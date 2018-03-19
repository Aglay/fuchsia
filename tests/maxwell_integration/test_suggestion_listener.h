// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_TESTS_MAXWELL_INTEGRATION_TEST_SUGGESTION_LISTENER_H_
#define PERIDOT_TESTS_MAXWELL_INTEGRATION_TEST_SUGGESTION_LISTENER_H_

#include <map>

#include "gtest/gtest.h"
#include "lib/fxl/logging.h"
#include "lib/suggestion/fidl/debug.fidl.h"
#include "lib/suggestion/fidl/suggestion_provider.fidl.h"

class TestSuggestionListener : public maxwell::NextListener,
                               public maxwell::QueryListener,
                               public maxwell::InterruptionListener {
 public:
  // |InterruptionListener|
  void OnInterrupt(maxwell::SuggestionPtr suggestion) override;

  // |NextListener|
  void OnNextResults(f1dl::VectorPtr<maxwell::SuggestionPtr> suggestions) override;

  // |NextListener|
  void OnProcessingChange(bool processing) override;

  // |QueryListener|
  void OnQueryResults(f1dl::VectorPtr<maxwell::SuggestionPtr> suggestions) override;

  // |QueryListener|
  void OnQueryComplete() override;

  int suggestion_count() const { return (signed)ordered_suggestions_.size(); }

  void ClearSuggestions();

  // Exposes a pointer to the only suggestion in this listener. Retains
  // ownership of the pointer.
  const maxwell::Suggestion* GetOnlySuggestion() const {
    EXPECT_EQ(1, suggestion_count());
    return GetTopSuggestion();
  }

  // Exposes a pointer to the top suggestion in this listener. Retains
  // ownership of the pointer.
  const maxwell::Suggestion* GetTopSuggestion() const {
    EXPECT_GE(suggestion_count(), 1);
    return ordered_suggestions_.front();
  }

  const maxwell::Suggestion* operator[](int index) const {
    return ordered_suggestions_[index];
  }

  const maxwell::Suggestion* operator[](const std::string& id) const {
    auto it = suggestions_by_id_.find(id);
    return it == suggestions_by_id_.end() ? NULL : it->second.get();
  }

  const std::vector<maxwell::Suggestion*>& GetSuggestions() {
    return ordered_suggestions_;
  }

 private:
  void OnAnyResults(f1dl::VectorPtr<maxwell::SuggestionPtr>& suggestions);

  std::map<std::string, maxwell::SuggestionPtr> suggestions_by_id_;
  std::vector<maxwell::Suggestion*> ordered_suggestions_;
};

class TestProposalListener {
 public:
  const std::vector<maxwell::ProposalSummaryPtr>& GetProposals() {
    return proposals_;
  }
  int proposal_count() const { return proposals_.size(); }

 protected:
  void UpdateProposals(f1dl::VectorPtr<maxwell::ProposalSummaryPtr> proposals) {
    proposals_.clear();
    for (size_t i = 0; i < proposals->size(); ++i) {
      proposals_.push_back(std::move(proposals->at(i)));
    }
  }
  std::vector<maxwell::ProposalSummaryPtr> proposals_;
};

class TestDebugNextListener : public maxwell::NextProposalListener,
                              public TestProposalListener {
 public:
  void OnNextUpdate(
      f1dl::VectorPtr<maxwell::ProposalSummaryPtr> proposals) override {
    FXL_LOG(INFO) << "In OnNextUpdate debug";
    UpdateProposals(std::move(proposals));
  }
};

class TestDebugAskListener : public maxwell::AskProposalListener,
                             public TestProposalListener {
 public:
  void OnAskStart(const f1dl::StringPtr& query,
                  f1dl::VectorPtr<maxwell::ProposalSummaryPtr> proposals) override {
    UpdateProposals(std::move(proposals));
    query_ = query.get();
  }
  void OnProposalSelected(
      maxwell::ProposalSummaryPtr selectedProposal) override {
    selected_proposal_ = selectedProposal.get();
  }
  const std::string get_query() { return query_; }
  maxwell::ProposalSummary* get_selected_proposal() {
    return selected_proposal_;
  }

 private:
  std::string query_;
  maxwell::ProposalSummary* selected_proposal_;
};

class TestDebugInterruptionListener
    : public maxwell::InterruptionProposalListener {
 public:
  void OnInterrupt(maxwell::ProposalSummaryPtr interruptionProposal) override {
    interrupt_proposal_ = std::move(interruptionProposal);
  }
  maxwell::ProposalSummaryPtr get_interrupt_proposal() {
    return interrupt_proposal_.Clone();
  }

 private:
  maxwell::ProposalSummaryPtr interrupt_proposal_;
};

#endif  // PERIDOT_TESTS_MAXWELL_INTEGRATION_TEST_SUGGESTION_LISTENER_H_
