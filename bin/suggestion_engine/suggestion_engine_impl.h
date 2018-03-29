// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SUGGESTION_ENGINE_SUGGESTION_ENGINE_IMPL_H_
#define PERIDOT_BIN_SUGGESTION_ENGINE_SUGGESTION_ENGINE_IMPL_H_

#include <map>
#include <string>
#include <vector>

#include "lib/app/cpp/application_context.h"

#include "peridot/bin/suggestion_engine/debug.h"
#include "peridot/bin/suggestion_engine/filter.h"
#include "peridot/bin/suggestion_engine/interruptions_processor.h"
#include "peridot/bin/suggestion_engine/next_processor.h"
#include "peridot/bin/suggestion_engine/proposal_publisher_impl.h"
#include "peridot/bin/suggestion_engine/query_handler_record.h"
#include "peridot/bin/suggestion_engine/query_processor.h"
#include "peridot/bin/suggestion_engine/ranked_suggestions_list.h"
#include "peridot/bin/suggestion_engine/suggestion_prototype.h"
#include "peridot/bin/suggestion_engine/timeline_stories_filter.h"
#include "peridot/bin/suggestion_engine/timeline_stories_watcher.h"
#include "peridot/lib/bound_set/bound_set.h"

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/media.h>

#include "lib/fidl/cpp/interface_ptr_set.h"
#include "lib/fxl/memory/weak_ptr.h"

namespace modular {

class ProposalPublisherImpl;

constexpr char kQueryContextKey[] = "/suggestion_engine/current_query";

// This class is currently responsible for 3 things:
//
// 1) Maintaining repositories of ranked Suggestions (stored inside
//    the RankedSuggestionsList class) for both Query and Next proposals.
//  a) Each query is handled by a separate instance of the QueryProcessor.
//
//     The set of Query proposals for the latest query are currently
//     buffered in the ask_suggestions_ member, though this process should
//     be made entirely stateless.
//
//  b) Next suggestions are issued by ProposalPublishers through the
//     Propose method, and can be issued at any time. These proposals
//     are stored in the next_suggestions_ member. The NextProcessor
//     handles all processing and notification of these proposals.
//
//  c) New next proposals are also considered for interruption. The
//     InterruptionProcessor examines proposals, decides whether they
//     should interruption, and, if so, makes further decisions about
//     when and how those interruptions should take place.
//
// 2) Storing the FIDL bindings for QueryHandlers and ProposalPublishers.
//
//  a) ProposalPublishers (for Next Suggestions) can be registered via the
//     RegisterProposalPublisher method.
//
//  b) QueryHandlers are currently registered through the
//     RegisterQueryHandler method.
//
// 3) Acts as a SuggestionProvider for those wishing to subscribe to
//    Suggestions.
class SuggestionEngineImpl : public ContextListener,
                             public SuggestionEngine,
                             public SuggestionProvider {
 public:
  SuggestionEngineImpl(component::ApplicationContext* app_context);
  ~SuggestionEngineImpl();

  fxl::WeakPtr<SuggestionDebugImpl> debug();

  // TODO(andrewosh): The following two methods should be removed. New
  // ProposalPublishers should be created whenever they're requested, and they
  // should be erased automatically when the client disconnects (they should be
  // stored in a BindingSet with an error handler that performs removal).
  void RemoveSourceClient(const std::string& component_url) {
    proposal_publishers_.erase(component_url);
  }

  // Should only be called from ProposalPublisherImpl.
  void AddNextProposal(ProposalPublisherImpl* source, Proposal proposal);

  // Should only be called from ProposalPublisherImpl.
  void RemoveNextProposal(const std::string& component_url,
                          const std::string& proposal_id);

  // |SuggestionProvider|
  void SubscribeToInterruptions(
      fidl::InterfaceHandle<InterruptionListener> listener) override;

  // |SuggestionProvider|
  void SubscribeToNext(fidl::InterfaceHandle<NextListener> listener,
                       int count) override;

  // |SuggestionProvider|
  void Query(fidl::InterfaceHandle<QueryListener> listener,
             UserInput input,
             int count) override;

  // |SuggestionProvider|
  void RegisterFeedbackListener(
      fidl::InterfaceHandle<FeedbackListener> speech_listener) override;

  // When a user interacts with a Suggestion, the suggestion engine will be
  // notified of consumed suggestion's ID. With this, we will do two things:
  //
  // 1) Perform the Action contained in the Suggestion
  //    (suggestion->proposal.on_selected)
  //
  //    Action handling should be extracted into separate classes to simplify
  //    SuggestionEngineImpl (i.e. an ActionManager which delegates action
  //    execution to ActionHandlers based on the Action's tag).
  //
  // 2) Remove consumed Suggestion from the next_suggestions_ repository,
  //    if it came from there.  Clear the ask_suggestions_ repository if
  //    it came from there.
  //
  // |SuggestionProvider|
  void NotifyInteraction(fidl::StringPtr suggestion_uuid,
                         Interaction interaction) override;

  // |SuggestionEngine|
  void RegisterProposalPublisher(
      fidl::StringPtr url,
      fidl::InterfaceRequest<ProposalPublisher> publisher) override;

  // |SuggestionEngine|
  void RegisterQueryHandler(
      fidl::StringPtr url,
      fidl::InterfaceHandle<QueryHandler> query_handler) override;

  // |SuggestionEngine|
  void Initialize(fidl::InterfaceHandle<modular::StoryProvider> story_provider,
                  fidl::InterfaceHandle<modular::FocusProvider> focus_provider,
                  fidl::InterfaceHandle<ContextWriter> context_writer,
                  fidl::InterfaceHandle<ContextReader> context_reader) override;

  // re-ranks dirty channels and dispatches updates
  void UpdateRanking();

  void Terminate(std::function<void()> done) { done(); }

 private:
  friend class InterruptionsProcessor;
  friend class NextProcessor;
  friend class QueryProcessor;

  // (proposer ID, proposal ID) => suggestion prototype
  using SuggestionPrototypeMap = std::map<std::pair<std::string, std::string>,
                                          std::unique_ptr<SuggestionPrototype>>;

  // Cleans up all resources associated with a query, including clearing
  // the previous ask suggestions, closing any still open SuggestionListeners,
  // etc.
  void CleanUpPreviousQuery();

  // Searches for a SuggestionPrototype in the Next and Ask lists.
  SuggestionPrototype* FindSuggestion(std::string suggestion_id);

  // Creates a suggestion prototype owned by the given |SuggestionPrototypeMap|.
  SuggestionPrototype* CreateSuggestionPrototype(SuggestionPrototypeMap* owner,
                                                 const std::string& source_url,
                                                 Proposal proposal);

  std::string RandomUuid() {
    static uint64_t id = 0;
    // TODO(rosswang): real UUIDs
    return std::to_string(id++);
  }

  // TODO(andrewosh): Performing actions should be handled by a separate
  // interface that's passed to the SuggestionEngineImpl.
  // |source_url| is the url of the source of the proposal containing the
  // provided actions.
  void PerformActions(fidl::VectorPtr<Action> actions,
                      const std::string& source_url,
                      uint32_t story_color);

  void PerformCreateStoryAction(const Action& action, uint32_t story_color);

  void PerformFocusStoryAction(const Action& action);

  // This call is deprecated, as the AddModuleToStory proposal action is
  // replaced by the AddModule action.
  void PerformAddModuleToStoryAction(const Action& action);

  void PerformAddModuleAction(const Action& action);

  void PerformCustomAction(Action* action,
                           const std::string& source_url,
                           uint32_t story_color);

  void RegisterRankingFeatures();

  void PlayMediaResponse(MediaResponsePtr media_response);
  void HandleMediaUpdates(uint64_t version,
                          media::MediaTimelineControlPointStatusPtr status);

  // |ContextListener|
  void OnContextUpdate(ContextUpdate update) override;

  fidl::BindingSet<SuggestionEngine> bindings_;
  fidl::BindingSet<SuggestionProvider> suggestion_provider_bindings_;
  fidl::BindingSet<SuggestionDebug> debug_bindings_;

  // Both story_provider_ and focus_provider_ptr are used exclusively during
  // Action execution (in the PerformActions call inside NotifyInteraction).
  //
  // These are required to create new Stories and interact with the current
  // Story.
  modular::StoryProviderPtr story_provider_;
  fidl::InterfacePtr<modular::FocusProvider> focus_provider_ptr_;

  // Watches for changes in StoryInfo from the StoryProvider, acts as a filter
  // for Proposals on all channels, and notifies when there are changes so that
  // we can re-filter Proposals.
  //
  // Initialized late in Initialize().
  std::unique_ptr<TimelineStoriesWatcher> timeline_stories_watcher_;

  // TODO(thatguy): All Channels also get a ReevaluateFilters method, which
  // would remove Suggestions that are now filtered or add
  // new ones that are no longer filtered.

  SuggestionPrototypeMap query_prototypes_;
  RankedSuggestionsList query_suggestions_;

  // next and interruptions share the same backing
  SuggestionPrototypeMap next_prototypes_;
  RankedSuggestionsList next_suggestions_;
  NextProcessor next_processor_;
  InterruptionsProcessor interruptions_processor_;

  // The set of all QueryHandlers that have been registered mapped to their
  // URLs (stored as strings).
  std::vector<QueryHandlerRecord> query_handlers_;

  std::map<std::string, std::shared_ptr<RankingFeature>> ranking_features;

  // The ProposalPublishers that have registered with the SuggestionEngine.
  std::map<std::string, std::unique_ptr<ProposalPublisherImpl>>
      proposal_publishers_;

  // TODO(andrewosh): Why is this necessary at this level?
  ProposalFilter filter_;

  // The ContextWriter that publishes the current user query to the
  // ContextEngine.
  ContextWriterPtr context_writer_;

  // The context reader that is used to rank suggestions using the current
  // context.
  ContextReaderPtr context_reader_;
  fidl::Binding<ContextListener> context_listener_binding_;

  // Latest context update received.
  ContextUpdatePtr latest_context_update_;

  std::unique_ptr<QueryProcessor> active_query_;

  media::AudioServerPtr audio_server_;
  media::MediaRendererPtr media_renderer_;
  media::MediaPacketProducerPtr media_packet_producer_;
  media::MediaTimelineControlPointPtr time_lord_;
  media::TimelineConsumerPtr media_timeline_consumer_;

  fidl::InterfacePtrSet<FeedbackListener> speech_listeners_;

  // The debugging interface for all Suggestions.
  SuggestionDebugImpl debug_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SuggestionEngineImpl);
};

}  // namespace modular

#endif  // PERIDOT_BIN_SUGGESTION_ENGINE_SUGGESTION_ENGINE_IMPL_H_
