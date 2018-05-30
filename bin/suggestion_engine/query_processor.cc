// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/query_processor.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "lib/fsl/tasks/message_loop.h"
#include "peridot/bin/suggestion_engine/suggestion_engine_helper.h"
#include "peridot/lib/fidl/json_xdr.h"

namespace modular {

namespace {

constexpr char kQueryContextKey[] = "/suggestion_engine/current_query";

}  // namespace

QueryProcessor::QueryProcessor(media::AudioServerPtr audio_server,
                               std::shared_ptr<SuggestionDebugImpl> debug)
    : debug_(debug), media_player_(std::move(audio_server), debug),
      has_media_response_(false) {
  media_player_.SetSpeechStatusCallback([this](SpeechStatus status) {
    NotifySpeechListeners(status);
  });
}

QueryProcessor::~QueryProcessor() = default;

void QueryProcessor::Initialize(
    fidl::InterfaceHandle<ContextWriter> context_writer) {
  context_writer_.Bind(std::move(context_writer));
}

void QueryProcessor::ExecuteQuery(
    UserInput input, int count, fidl::InterfaceHandle<QueryListener> listener) {
  // TODO(jwnichols): I'm not sure this is correct or should be here
  NotifySpeechListeners(SpeechStatus::PROCESSING);

  // Process:
  //   1. Close out and clean up any existing query process
  //   2. Update the context engine with the new query
  //   3. Set up the ask variables in suggestion engine
  //   4. Get suggestions from each of the QueryHandlers
  //   5. Filter and Rank the suggestions as received
  //   6. Send "done" to SuggestionListener

  // Step 1
  CleanUpPreviousQuery();

  // Step 2
  std::string query = input.text;
  if (!query.empty() && context_writer_.is_bound()) {
    // Update context engine
    std::string formattedQuery;

    modular::XdrFilterType<std::string> filter_list[] = {
      modular::XdrFilter<std::string>,
      nullptr,
    };

    modular::XdrWrite(&formattedQuery, &query, filter_list);
    context_writer_->WriteEntityTopic(kQueryContextKey, formattedQuery);

    // Update suggestion engine debug interface
    debug_->OnAskStart(query, &suggestions_);
  }

  // Steps 3 - 6
  activity_ = debug_->GetIdleWaiter()->RegisterOngoingActivity();
  active_query_ = std::make_unique<QueryRunner>(
      std::move(listener), std::move(input), count);
  active_query_->SetResponseCallback([this, input](
      const std::string& handler_url, QueryResponse response) {
    OnQueryResponse(input, handler_url, std::move(response));
  });
  active_query_->SetEndRequestCallback([this, input] {
    OnQueryEndRequest(input);
  });
  active_query_->Run(query_handlers_);
}

void QueryProcessor::RegisterFeedbackListener(
    fidl::InterfaceHandle<FeedbackListener> speech_listener) {
  speech_listeners_.AddInterfacePtr(speech_listener.Bind());
}

void QueryProcessor::RegisterQueryHandler(
    fidl::StringPtr url,
    fidl::InterfaceHandle<QueryHandler> query_handler_handle) {
  auto query_handler = query_handler_handle.Bind();
  query_handlers_.emplace_back(std::move(query_handler), url);
}

void QueryProcessor::SetFilters(
    std::vector<std::unique_ptr<SuggestionActiveFilter>>&& active_filters,
    std::vector<std::unique_ptr<SuggestionPassiveFilter>>&& passive_filters) {
  suggestions_.SetActiveFilters(std::move(active_filters));
  suggestions_.SetPassiveFilters(std::move(passive_filters));
}

void QueryProcessor::SetRanker(std::unique_ptr<Ranker> ranker) {
  suggestions_.SetRanker(std::move(ranker));
}

RankedSuggestion* QueryProcessor::GetSuggestion(
    const std::string& suggestion_uuid) const {
  return suggestions_.GetSuggestion(suggestion_uuid);
}

void QueryProcessor::CleanUpPreviousQuery() {
  has_media_response_ = false;
  active_query_.reset();
  suggestions_.RemoveAllSuggestions();
}

void QueryProcessor::AddProposal(const std::string& source_url,
                                 Proposal proposal) {
  suggestions_.RemoveProposal(source_url, proposal.id);

  auto suggestion = CreateSuggestionPrototype(
      &query_prototypes_, source_url,
      "" /* Emtpy story_id */,
      std::move(proposal));
  suggestions_.AddSuggestion(std::move(suggestion));
}

void QueryProcessor::NotifySpeechListeners(SpeechStatus status) {
  for (auto& speech_listener : speech_listeners_.ptrs()) {
    (*speech_listener)->OnStatusChanged(SpeechStatus::PROCESSING);
  }
}

void QueryProcessor::OnQueryResponse(
    UserInput input, const std::string& handler_url, QueryResponse response) {
  // TODO(rosswang): defer selection of "I don't know" responses
  if (!has_media_response_ && response.media_response) {
    has_media_response_ = true;

    // TODO(rosswang): Wait for other potential voice responses so that we
    // choose the best one. We don't have criteria for "best" yet, and we only
    // have one agent (Kronk) with voice responses now, so play immediately.

    // TODO(rosswang): allow falling back on natural language text response
    // without a spoken response
    fidl::StringPtr text_response =
        std::move(response.natural_language_response);
    if (!text_response) {
      text_response = "";
    }
    for (auto& listener : speech_listeners_.ptrs()) {
      (*listener)->OnTextResponse(text_response);
    }

    media_player_.PlayMediaResponse(std::move(response.media_response));
  }

  // Ranking currently happens as each set of proposals are added.
  for (size_t i = 0; i < response.proposals->size(); ++i) {
    AddProposal(handler_url, std::move(response.proposals->at(i)));
  }
  suggestions_.Refresh(input);

  // Update the QueryListener with new results
  NotifyOfResults();

  // Update the suggestion engine debug interface
  debug_->OnAskStart(input.text, &suggestions_);
}

void QueryProcessor::OnQueryEndRequest(UserInput input) {
  debug_->OnAskStart(input.text, &suggestions_);
  if (!has_media_response_) {
    // there was no media response for this query, so idle immediately
    NotifySpeechListeners(SpeechStatus::IDLE);
  }
  activity_ = nullptr;
}

void QueryProcessor::NotifyOfResults() {
  const auto& suggestion_vector = suggestions_.Get();

  fidl::VectorPtr<Suggestion> window;
  for (size_t i = 0;
       i < active_query_->max_results() && i < suggestion_vector.size(); i++) {
    window.push_back(CreateSuggestion(*suggestion_vector[i]));
  }

  if (window) {
    active_query_->listener()->OnQueryResults(std::move(window));
  }
}

}  // namespace modular
