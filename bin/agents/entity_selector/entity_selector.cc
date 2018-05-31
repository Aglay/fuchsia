// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include "lib/app/cpp/startup_context.h"
#include "lib/context/cpp/context_helper.h"
#include "lib/fsl/tasks/message_loop.h"
#include "peridot/bin/agents/entity_utils/entity_span.h"
#include "peridot/bin/agents/entity_utils/entity_utils.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "third_party/rapidjson/rapidjson/document.h"
#include "third_party/rapidjson/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/rapidjson/writer.h"

namespace fuchsia {
namespace modular {

// Subscribe to entities and selection in the Context Engine, and Publish any
// selected entities back to the Context Engine.
class SelectedEntityFinder : ContextListener {
 public:
  SelectedEntityFinder()
      : context_(component::StartupContext::CreateFromStartupInfo()),
        reader_(context_->ConnectToEnvironmentService<ContextReader>()),
        writer_(context_->ConnectToEnvironmentService<ContextWriter>()),
        binding_(this) {
    ContextQuery query;
    for (const std::string& topic :
         {kFocalEntitiesTopic, kRawTextSelectionTopic}) {
      ContextSelector selector;
      selector.type = ContextValueType::ENTITY;
      selector.meta = ContextMetadata::New();
      selector.meta->entity = EntityMetadata::New();
      selector.meta->entity->topic = topic;
      AddToContextQuery(&query, topic, std::move(selector));
    }
    reader_->Subscribe(std::move(query), binding_.NewBinding());
  }

 private:
  // Parse a JSON representation of selection.
  std::pair<int, int> GetSelectionFromJson(const std::string& json_string) {
    // Validate and parse the string.
    if (json_string.empty()) {
      FXL_LOG(INFO) << "No current selection.";
      return std::make_pair(-1, -1);
    }
    rapidjson::Document selection;
    selection.Parse(json_string);
    if (selection.HasParseError() || selection.Empty()) {
      FXL_LOG(ERROR) << "Invalid " << kRawTextSelectionTopic
                     << " entry in Context." << json_string;
      return std::make_pair(-1, -1);
    }
    if (!(selection.HasMember("start") && selection["start"].IsInt() &&
          selection.HasMember("end") && selection["end"].IsInt())) {
      FXL_LOG(ERROR) << "Invalid " << kRawTextSelectionTopic
                     << " entry in Context. "
                     << "Missing \"start\" or \"end\" keys.";
      return std::make_pair(-1, -1);
    }

    const int start = selection["start"].GetInt();
    const int end = selection["end"].GetInt();
    return std::make_pair(start, end);
  }

  // Return a JSON representation of an array of entities that fall within
  // start and end.
  std::string GetSelectedEntities(const std::vector<EntitySpan>& entities,
                                  const int selection_start,
                                  const int selection_end) {
    rapidjson::Document d;
    rapidjson::Value entities_json(rapidjson::kArrayType);
    for (const EntitySpan& e : entities) {
      if (e.GetStart() <= selection_start && e.GetEnd() >= selection_end) {
        d.Parse(e.GetJsonString());
        entities_json.PushBack(d, d.GetAllocator());
      }
    }
    return fuchsia::modular::JsonValueToString(entities_json);
  }

  // |ContextListener|
  void OnContextUpdate(ContextUpdate result) override {
    auto focal_entities = TakeContextValue(&result, kFocalEntitiesTopic);
    auto text_selection = TakeContextValue(&result, kRawTextSelectionTopic);
    if (!focal_entities.first || !text_selection.first ||
        focal_entities.second->empty() || text_selection.second->empty()) {
      return;
    }
    const std::vector<EntitySpan> entities =
        EntitySpan::FromContextValues(focal_entities.second);
    const std::pair<int, int> start_and_end =
        GetSelectionFromJson(text_selection.second->at(0).content);
    writer_->WriteEntityTopic(kSelectedEntitiesTopic,
                              GetSelectedEntities(entities, start_and_end.first,
                                                  start_and_end.second));
  }

  std::unique_ptr<component::StartupContext> context_;
  ContextReaderPtr reader_;
  ContextWriterPtr writer_;
  fidl::Binding<ContextListener> binding_;
};

}  // namespace modular
}  // namespace fuchsia

int main(int argc, const char** argv) {
  fsl::MessageLoop loop;
  fuchsia::modular::SelectedEntityFinder app;
  loop.Run();
  return 0;
}
