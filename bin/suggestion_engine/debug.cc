// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <functional>

#include "peridot/bin/suggestion_engine/debug.h"

namespace maxwell {

SuggestionDebugImpl::SuggestionDebugImpl() = default;
SuggestionDebugImpl::~SuggestionDebugImpl() = default;

void makeProposalSummary(const SuggestionPrototype* suggestion,
                         ProposalSummaryPtr* summary) {
  (*summary)->id = suggestion->proposal->id;
  (*summary)->publisher_url = suggestion->source_url;
  (*summary)->display = suggestion->proposal->display.Clone();
}

void makeProposalSummaries(const RankedSuggestionsList* suggestions,
                           f1dl::VectorPtr<ProposalSummaryPtr>* summaries) {
  for (const auto& suggestion : suggestions->Get()) {
    ProposalSummaryPtr summary = ProposalSummary::New();
    makeProposalSummary(suggestion->prototype, &summary);
    summaries->push_back(std::move(summary));
  }
}

void SuggestionDebugImpl::OnAskStart(std::string query,
                                     const RankedSuggestionsList* suggestions) {
  ask_proposal_listeners_.ForAllPtrs(
      [query, suggestions](AskProposalListener* listener) {
        auto proposals = f1dl::VectorPtr<ProposalSummaryPtr>::New(0);
        makeProposalSummaries(suggestions, &proposals);
        listener->OnAskStart(query, std::move(proposals));
      });
}

void SuggestionDebugImpl::OnSuggestionSelected(
    const SuggestionPrototype* selected_suggestion) {
  ask_proposal_listeners_.ForAllPtrs(
      [selected_suggestion](AskProposalListener* listener) {
        if (selected_suggestion) {
          ProposalSummaryPtr summary = ProposalSummary::New();
          makeProposalSummary(selected_suggestion, &summary);
          listener->OnProposalSelected(std::move(summary));
        } else {
          listener->OnProposalSelected(nullptr);
        }
      });
}

void SuggestionDebugImpl::OnInterrupt(
    const SuggestionPrototype* interrupt_suggestion) {
  interruption_proposal_listeners_.ForAllPtrs(
      [interrupt_suggestion](InterruptionProposalListener* listener) {
        ProposalSummaryPtr summary = ProposalSummary::New();
        makeProposalSummary(interrupt_suggestion, &summary);
        listener->OnInterrupt(std::move(summary));
      });
}

void SuggestionDebugImpl::OnNextUpdate(
    const RankedSuggestionsList* suggestions) {
  next_proposal_listeners_.ForAllPtrs(
      [this, suggestions](NextProposalListener* listener) {
        auto proposals = f1dl::VectorPtr<ProposalSummaryPtr>::New(0);
        makeProposalSummaries(suggestions, &proposals);
        listener->OnNextUpdate(std::move(proposals));
        cached_next_proposals_ = std::move(proposals);
      });
}

void SuggestionDebugImpl::WatchAskProposals(
    f1dl::InterfaceHandle<AskProposalListener> listener) {
  auto listener_ptr = listener.Bind();
  ask_proposal_listeners_.AddInterfacePtr(std::move(listener_ptr));
}

void SuggestionDebugImpl::WatchInterruptionProposals(
    f1dl::InterfaceHandle<InterruptionProposalListener> listener) {
  auto listener_ptr = listener.Bind();
  interruption_proposal_listeners_.AddInterfacePtr(std::move(listener_ptr));
}

void SuggestionDebugImpl::WatchNextProposals(
    f1dl::InterfaceHandle<NextProposalListener> listener) {
  auto listener_ptr = listener.Bind();
  next_proposal_listeners_.AddInterfacePtr(std::move(listener_ptr));
  if (cached_next_proposals_) {
    listener_ptr->OnNextUpdate(std::move(cached_next_proposals_));
  }
}

}  // namespace maxwell
