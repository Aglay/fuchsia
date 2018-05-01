// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <functional>

#include "lib/fidl/cpp/optional.h"
#include "peridot/bin/suggestion_engine/debug.h"

namespace modular {

SuggestionDebugImpl::SuggestionDebugImpl() : weak_ptr_factory_(this){};
SuggestionDebugImpl::~SuggestionDebugImpl() = default;

fxl::WeakPtr<SuggestionDebugImpl> SuggestionDebugImpl::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

void makeProposalSummary(const SuggestionPrototype* suggestion,
                         ProposalSummary* summary) {
  summary->id = suggestion->proposal.id;
  summary->publisher_url = suggestion->source_url;
  fidl::Clone(suggestion->proposal.display, &summary->display);
}

void makeProposalSummaries(const RankedSuggestionsList* suggestions,
                           fidl::VectorPtr<ProposalSummary>* summaries) {
  for (const auto& suggestion : suggestions->Get()) {
    ProposalSummary summary;
    makeProposalSummary(suggestion->prototype, &summary);
    summaries->push_back(std::move(summary));
  }
}

void SuggestionDebugImpl::OnAskStart(std::string query,
                                     const RankedSuggestionsList* suggestions) {
  for (auto& listener : ask_proposal_listeners_.ptrs()) {
    auto proposals = fidl::VectorPtr<ProposalSummary>::New(0);
    makeProposalSummaries(suggestions, &proposals);
    (*listener)->OnAskStart(query, std::move(proposals));
  }
}

void SuggestionDebugImpl::OnSuggestionSelected(
    const SuggestionPrototype* selected_suggestion) {
  for (auto& listener : ask_proposal_listeners_.ptrs()) {
    if (selected_suggestion) {
      auto summary = ProposalSummary::New();
      makeProposalSummary(selected_suggestion, summary.get());
      (*listener)->OnProposalSelected(std::move(summary));
    } else {
      (*listener)->OnProposalSelected(nullptr);
    }
  }
}

void SuggestionDebugImpl::OnInterrupt(
    const SuggestionPrototype* interrupt_suggestion) {
  for (auto& listener : interruption_proposal_listeners_.ptrs()) {
    ProposalSummary summary;
    makeProposalSummary(interrupt_suggestion, &summary);
    (*listener)->OnInterrupt(std::move(summary));
  }
}

void SuggestionDebugImpl::OnNextUpdate(
    const RankedSuggestionsList* suggestions) {
  for (auto& listener : next_proposal_listeners_.ptrs()) {
    auto proposals = fidl::VectorPtr<ProposalSummary>::New(0);
    makeProposalSummaries(suggestions, &proposals);
    (*listener)->OnNextUpdate(std::move(proposals));
    cached_next_proposals_ = std::move(proposals);
  }
}

util::IdleWaiter* SuggestionDebugImpl::GetIdleWaiter() {
  return &wait_until_idle_;
}

void SuggestionDebugImpl::WatchAskProposals(
    fidl::InterfaceHandle<AskProposalListener> listener) {
  auto listener_ptr = listener.Bind();
  ask_proposal_listeners_.AddInterfacePtr(std::move(listener_ptr));
}

void SuggestionDebugImpl::WatchInterruptionProposals(
    fidl::InterfaceHandle<InterruptionProposalListener> listener) {
  auto listener_ptr = listener.Bind();
  interruption_proposal_listeners_.AddInterfacePtr(std::move(listener_ptr));
}

void SuggestionDebugImpl::WatchNextProposals(
    fidl::InterfaceHandle<NextProposalListener> listener) {
  auto listener_ptr = listener.Bind();
  next_proposal_listeners_.AddInterfacePtr(std::move(listener_ptr));
  if (cached_next_proposals_) {
    listener_ptr->OnNextUpdate(std::move(cached_next_proposals_));
  }
}

void SuggestionDebugImpl::WaitUntilIdle(WaitUntilIdleCallback callback) {
  wait_until_idle_.WaitUntilIdle(callback);
}

}  // namespace modular
