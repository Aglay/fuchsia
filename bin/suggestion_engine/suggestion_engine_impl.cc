// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/suggestion_engine_impl.h"
#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/time/time_delta.h"
#include "lib/fxl/time/time_point.h"
#include "lib/media/timeline/timeline.h"
#include "lib/media/timeline/timeline_rate.h"
#include "lib/suggestion/fidl/suggestion_engine.fidl.h"
#include "lib/suggestion/fidl/user_input.fidl.h"
#include "peridot/bin/suggestion_engine/ranking_feature.h"
#include "peridot/bin/suggestion_engine/ranking_features/kronk_ranking_feature.h"
#include "peridot/bin/suggestion_engine/ranking_features/proposal_hint_ranking_feature.h"
#include "peridot/bin/suggestion_engine/ranking_features/query_match_ranking_feature.h"
#include "peridot/lib/fidl/json_xdr.h"

#include <string>

namespace maxwell {

SuggestionEngineImpl::SuggestionEngineImpl(app::ApplicationContext* app_context)
    : next_processor_(this) {
  app_context->outgoing_services()->AddService<SuggestionEngine>(
      [this](f1dl::InterfaceRequest<SuggestionEngine> request) {
        bindings_.AddBinding(this, std::move(request));
      });
  app_context->outgoing_services()->AddService<SuggestionProvider>(
      [this](f1dl::InterfaceRequest<SuggestionProvider> request) {
        suggestion_provider_bindings_.AddBinding(this, std::move(request));
      });
  app_context->outgoing_services()->AddService<SuggestionDebug>(
      [this](f1dl::InterfaceRequest<SuggestionDebug> request) {
        debug_bindings_.AddBinding(&debug_, std::move(request));
      });

  media_service_ =
      app_context->ConnectToEnvironmentService<media::MediaService>();
  media_service_.set_error_handler([this] {
    FXL_LOG(INFO) << "Media service connection error";
    media_service_ = nullptr;
    media_packet_producer_ = nullptr;
  });

  // Create common ranking features
  std::shared_ptr<RankingFeature> proposal_hint_feature =
      std::make_shared<ProposalHintRankingFeature>();
  std::shared_ptr<RankingFeature> kronk_feature =
      std::make_shared<KronkRankingFeature>();

  // TODO(jwnichols): Replace the code configuration of the ranking features
  // with a configuration file

  // Set up the next ranking features
  next_suggestions_.AddRankingFeature(1.0, proposal_hint_feature);
  next_suggestions_.AddRankingFeature(-0.1, kronk_feature);

  // Set up the query ranking features
  ask_suggestions_.AddRankingFeature(1.0, proposal_hint_feature);
  ask_suggestions_.AddRankingFeature(-0.1, kronk_feature);
  ask_suggestions_.AddRankingFeature(
      0, std::make_shared<QueryMatchRankingFeature>());
}

SuggestionEngineImpl::~SuggestionEngineImpl() = default;

void SuggestionEngineImpl::AddNextProposal(ProposalPublisherImpl* source,
                                           ProposalPtr proposal) {
  next_processor_.AddProposal(source->component_url(), std::move(proposal));
}

void SuggestionEngineImpl::RemoveProposal(const std::string& component_url,
                                          const std::string& proposal_id) {
  const auto key = std::make_pair(component_url, proposal_id);
  auto toRemove = suggestion_prototypes_.find(key);
  if (toRemove != suggestion_prototypes_.end()) {
    if (active_query_ != nullptr)
      active_query_->RemoveProposal(component_url, proposal_id);
    next_processor_.RemoveProposal(component_url, proposal_id);
    suggestion_prototypes_.erase(toRemove);
  }
}

// |SuggestionProvider|
void SuggestionEngineImpl::Query(f1dl::InterfaceHandle<QueryListener> listener,
                                 UserInputPtr input,
                                 int count) {
  // TODO(jwnichols): I'm not sure this is correct or should be here
  speech_listeners_.ForAllPtrs([](FeedbackListener* listener) {
    listener->OnStatusChanged(SpeechStatus::PROCESSING);
  });

  // Process:
  //   1. Close out and clean up any existing query process
  //   2. Update the context engine with the new query
  //   3. Set up the ask variables in suggestion engine
  //   4. Get suggestions from each of the QueryHandlers
  //   5. Rank the suggestions as received
  //   6. Send "done" to SuggestionListener

  // Step 1
  CleanUpPreviousQuery();

  // Step 2
  std::string query = input->text;
  if (!query.empty()) {
    // Update context engine
    std::string formattedQuery;
    modular::XdrWrite(&formattedQuery, &query, modular::XdrFilter<std::string>);
    context_writer_->WriteEntityTopic(kQueryContextKey, formattedQuery);

    // Update suggestion engine debug interface
    debug_.OnAskStart(query, &ask_suggestions_);
  }

  // Steps 3 - 6
  active_query_ = std::make_unique<QueryProcessor>(this, std::move(listener),
                                                   std::move(input), count);
}

void SuggestionEngineImpl::Validate() {
  next_processor_.Validate();
}

// |SuggestionProvider|
void SuggestionEngineImpl::SubscribeToInterruptions(
    f1dl::InterfaceHandle<InterruptionListener> listener) {
  interruptions_processor_.RegisterListener(std::move(listener));
}

// |SuggestionProvider|
void SuggestionEngineImpl::SubscribeToNext(
    f1dl::InterfaceHandle<NextListener> listener,
    int count) {
  next_processor_.RegisterListener(std::move(listener), count);
}

// |SuggestionProvider|
void SuggestionEngineImpl::RegisterFeedbackListener(
    f1dl::InterfaceHandle<FeedbackListener> speech_listener) {
  speech_listeners_.AddInterfacePtr(speech_listener.Bind());
}

// |SuggestionProvider|
void SuggestionEngineImpl::NotifyInteraction(
    const f1dl::String& suggestion_uuid,
    InteractionPtr interaction) {
  // Find the suggestion
  bool suggestion_in_ask = false;
  RankedSuggestion* suggestion =
      next_suggestions_.GetSuggestion(suggestion_uuid);
  if (!suggestion) {
    suggestion = ask_suggestions_.GetSuggestion(suggestion_uuid);
    suggestion_in_ask = true;
  }

  // If it exists (and it should), perform the action and clean up
  if (suggestion) {
    std::string log_detail = suggestion->prototype
                                 ? short_proposal_str(*suggestion->prototype)
                                 : "invalid";

    FXL_LOG(INFO) << (interaction->type == InteractionType::SELECTED
                          ? "Accepted"
                          : "Dismissed")
                  << " suggestion " << suggestion_uuid << " (" << log_detail
                  << ")";

    debug_.OnSuggestionSelected(suggestion->prototype);

    auto& proposal = suggestion->prototype->proposal;
    if (interaction->type == InteractionType::SELECTED) {
      PerformActions(proposal->on_selected, suggestion->prototype->source_url,
                     proposal->display->color);
    }

    if (suggestion_in_ask) {
      CleanUpPreviousQuery();
    } else {
      RemoveProposal(suggestion->prototype->source_url, proposal->id);
    }

    Validate();
  } else {
    FXL_LOG(WARNING) << "Requested suggestion prototype not found. UUID: "
                     << suggestion_uuid;
  }
}

// |SuggestionEngine|
void SuggestionEngineImpl::RegisterProposalPublisher(
    const f1dl::String& url,
    f1dl::InterfaceRequest<ProposalPublisher> publisher) {
  // Check to see if a ProposalPublisher has already been created for the
  // component with this url. If not, create one.
  std::unique_ptr<ProposalPublisherImpl>& source = proposal_publishers_[url];
  if (!source) {  // create if it didn't already exist
    source = std::make_unique<ProposalPublisherImpl>(this, url);
  }

  source->AddBinding(std::move(publisher));
}

// |SuggestionEngine|
void SuggestionEngineImpl::RegisterQueryHandler(
    const f1dl::String& url,
    f1dl::InterfaceHandle<QueryHandler> query_handler_handle) {
  auto query_handler = query_handler_handle.Bind();
  query_handlers_.emplace_back(std::move(query_handler), url);
}

// |SuggestionEngine|
void SuggestionEngineImpl::Initialize(
    f1dl::InterfaceHandle<modular::StoryProvider> story_provider,
    f1dl::InterfaceHandle<modular::FocusProvider> focus_provider,
    f1dl::InterfaceHandle<ContextWriter> context_writer) {
  story_provider_.Bind(std::move(story_provider));
  focus_provider_ptr_.Bind(std::move(focus_provider));
  context_writer_.Bind(std::move(context_writer));

  timeline_stories_watcher_.reset(new TimelineStoriesWatcher(&story_provider_));
}

// end SuggestionEngine

void SuggestionEngineImpl::CleanUpPreviousQuery() {
  // Clean up the query processor
  active_query_.reset();

  // Clean up the suggestions
  for (auto& suggestion : ask_suggestions_.Get()) {
    suggestion_prototypes_.erase(
        std::make_pair(suggestion->prototype->source_url,
                       suggestion->prototype->proposal->id));
  }
  ask_suggestions_.RemoveAllSuggestions();
}

SuggestionPrototype* SuggestionEngineImpl::CreateSuggestionPrototype(
    const std::string& source_url,
    ProposalPtr proposal) {
  auto prototype_pair =
      suggestion_prototypes_.emplace(std::make_pair(source_url, proposal->id),
                                     std::make_unique<SuggestionPrototype>());
  auto suggestion_prototype = prototype_pair.first->second.get();
  suggestion_prototype->suggestion_id = RandomUuid();
  suggestion_prototype->source_url = source_url;
  suggestion_prototype->timestamp = fxl::TimePoint::Now();
  suggestion_prototype->proposal = std::move(proposal);

  return suggestion_prototype;
}

void SuggestionEngineImpl::PerformActions(
    const f1dl::Array<maxwell::ActionPtr>& actions,
    const std::string& source_url,
    uint32_t story_color) {
  // TODO(rosswang): If we're asked to add multiple modules, we probably
  // want to add them to the same story. We can't do that yet, but we need
  // to receive a StoryController anyway (not optional atm.).
  for (const auto& action : actions) {
    switch (action->which()) {
      case Action::Tag::CREATE_STORY: {
        PerformCreateStoryAction(action, story_color);
        break;
      }
      case Action::Tag::FOCUS_STORY: {
        PerformFocusStoryAction(action);
        break;
      }
      case Action::Tag::ADD_MODULE_TO_STORY: {
        PerformAddModuleToStoryAction(action);
        break;
      }
      case Action::Tag::ADD_MODULE: {
        PerformAddModuleAction(action, source_url);
        break;
      }
      case Action::Tag::CUSTOM_ACTION: {
        PerformCustomAction(action, source_url, story_color);
        break;
      }
      default:
        FXL_LOG(WARNING) << "Unknown action tag " << (uint32_t)action->which();
    }
  }
}

void SuggestionEngineImpl::PerformCreateStoryAction(const ActionPtr& action,
                                                    uint32_t story_color) {
  const auto& create_story = action->get_create_story();

  if (story_provider_) {
    // TODO(afergan): Make this more robust later. For now, we
    // always assume that there's extra info and that it's a color.
    f1dl::Map<f1dl::String, f1dl::String> extra_info;
    char hex_color[11];
    sprintf(hex_color, "0x%x", story_color);
    extra_info["color"] = hex_color;
    auto& initial_data = create_story->initial_data;
    auto& module_id = create_story->module_id;
    story_provider_->CreateStoryWithInfo(
        create_story->module_id, std::move(extra_info), std::move(initial_data),
        [this, module_id](const f1dl::String& story_id) {
          modular::StoryControllerPtr story_controller;
          story_provider_->GetController(story_id,
                                         story_controller.NewRequest());
          FXL_LOG(INFO) << "Creating story with module " << module_id;

          story_controller->GetInfo(fxl::MakeCopyable(
              // TODO(thatguy): We should not be std::move()ing
              // story_controller *while we're calling it*.
              [this, controller = std::move(story_controller)](
                  modular::StoryInfoPtr story_info, modular::StoryState state) {
                FXL_LOG(INFO)
                    << "Requesting focus for story_id " << story_info->id;
                focus_provider_ptr_->Request(story_info->id);
              }));
        });
  } else {
    FXL_LOG(WARNING) << "Unable to add module; no story provider";
  }
}

void SuggestionEngineImpl::PerformFocusStoryAction(const ActionPtr& action) {
  const auto& focus_story = action->get_focus_story();
  FXL_LOG(INFO) << "Requesting focus for story_id " << focus_story->story_id;
  focus_provider_ptr_->Request(focus_story->story_id);
}

void SuggestionEngineImpl::PerformAddModuleToStoryAction(
    const ActionPtr& action) {
  if (story_provider_) {
    const auto& add_module_to_story = action->get_add_module_to_story();
    const auto& story_id = add_module_to_story->story_id;
    const auto& module_name = add_module_to_story->module_name;
    const auto& module_url = add_module_to_story->module_url;
    const auto& link_name = add_module_to_story->link_name;
    const auto& module_path = add_module_to_story->module_path;
    const auto& surface_relation = add_module_to_story->surface_relation;

    FXL_LOG(INFO) << "Adding module " << module_url << " to story " << story_id;

    modular::StoryControllerPtr story_controller;
    story_provider_->GetController(story_id, story_controller.NewRequest());
    if (!add_module_to_story->initial_data.is_null()) {
      modular::LinkPtr link;
      story_controller->GetLink(module_path.Clone(), link_name,
                                link.NewRequest());
      link->Set(nullptr /* json_path */, add_module_to_story->initial_data);
    }

    story_controller->AddModule(module_path.Clone(), module_name, module_url,
                                link_name, surface_relation.Clone());
  } else {
    FXL_LOG(WARNING) << "Unable to add module; no story provider";
  }
}

void SuggestionEngineImpl::PerformAddModuleAction(
    const ActionPtr& action,
    const std::string& source_url) {
  if (story_provider_) {
    const auto& add_module = action->get_add_module();
    const auto& module_name = add_module->module_name;
    const auto& story_id = add_module->story_id;
    modular::StoryControllerPtr story_controller;
    story_provider_->GetController(story_id, story_controller.NewRequest());
    story_controller->AddDaisy({source_url}, module_name,
                               add_module->daisy.Clone(),
                               add_module->surface_relation.Clone());
  } else {
    FXL_LOG(WARNING) << "Unable to add module; no story provider";
  }
}

void SuggestionEngineImpl::PerformCustomAction(const ActionPtr& action,
                                               const std::string& source_url,
                                               uint32_t story_color) {
  auto custom_action = action->get_custom_action().Bind();
  custom_action->Execute(fxl::MakeCopyable(
      [this, custom_action = std::move(custom_action), source_url,
       story_color](f1dl::Array<maxwell::ActionPtr> actions) {
        if (actions)
          PerformActions(std::move(actions), source_url, story_color);
      }));
}

void SuggestionEngineImpl::PlayMediaResponse(MediaResponsePtr media_response) {
  if (!media_service_)
    return;

  media::AudioRendererPtr audio_renderer;
  media::MediaRendererPtr media_renderer;
  media_service_->CreateAudioRenderer(audio_renderer.NewRequest(),
                                      media_renderer.NewRequest());

  media_sink_.Unbind();
  media_service_->CreateSink(media_renderer.Unbind(), media_sink_.NewRequest());

  media_packet_producer_ = media_response->media_packet_producer.Bind();
  media_sink_->ConsumeMediaType(
      std::move(media_response->media_type),
      [this](f1dl::InterfaceHandle<media::MediaPacketConsumer> consumer) {
        media_packet_producer_->Connect(consumer.Bind(), [this] {
          time_lord_.Unbind();
          media_timeline_consumer_.Unbind();

          speech_listeners_.ForAllPtrs([](FeedbackListener* listener) {
            listener->OnStatusChanged(SpeechStatus::RESPONDING);
          });

          media_sink_->GetTimelineControlPoint(time_lord_.NewRequest());
          time_lord_->GetTimelineConsumer(
              media_timeline_consumer_.NewRequest());
          time_lord_->Prime([this] {
            auto tt = media::TimelineTransform::New();
            tt->reference_time =
                media::Timeline::local_now() + media::Timeline::ns_from_ms(30);
            tt->subject_time = media::kUnspecifiedTime;
            tt->reference_delta = tt->subject_delta = 1;

            HandleMediaUpdates(media::MediaTimelineControlPoint::kInitialStatus,
                               nullptr);

            media_timeline_consumer_->SetTimelineTransform(
                std::move(tt), [](bool completed) {});
          });
        });
      });

  media_packet_producer_.set_error_handler([this] {
    speech_listeners_.ForAllPtrs([](FeedbackListener* listener) {
      listener->OnStatusChanged(SpeechStatus::IDLE);
    });
  });
}

void SuggestionEngineImpl::HandleMediaUpdates(
    uint64_t version,
    media::MediaTimelineControlPointStatusPtr status) {
  if (status && status->end_of_stream) {
    speech_listeners_.ForAllPtrs([](FeedbackListener* listener) {
      listener->OnStatusChanged(SpeechStatus::IDLE);
    });
    media_packet_producer_ = nullptr;
    media_sink_ = nullptr;
  } else {
    time_lord_->GetStatus(
        version, [this](uint64_t next_version,
                        media::MediaTimelineControlPointStatusPtr next_status) {
          HandleMediaUpdates(next_version, std::move(next_status));
        });
  }
}

}  // namespace maxwell

int main(int argc, const char** argv) {
  fsl::MessageLoop loop;
  auto context = app::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<maxwell::SuggestionEngineImpl> driver(
      context->outgoing_services(),
      std::make_unique<maxwell::SuggestionEngineImpl>(context.get()),
      [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
