// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <regex>
#include <vector>

#include <fuchsia/modular/cpp/fidl.h>
#include "lib/app/cpp/application_context.h"
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

const std::string kEmailRegex = "[^\\s]+@[^\\s]+";

// Subscribe to the Context Engine and Publish any entities found back to
// the Context Engine.
class BasicTextListener : ContextListener {
 public:
  BasicTextListener()
      : app_context_(component::ApplicationContext::CreateFromStartupInfo()),
        reader_(app_context_->ConnectToEnvironmentService<ContextReader>()),
        writer_(app_context_->ConnectToEnvironmentService<ContextWriter>()),
        binding_(this) {
    ContextSelector selector;
    selector.type = ContextValueType::ENTITY;
    selector.meta = ContextMetadata::New();
    selector.meta->entity = EntityMetadata::New();
    selector.meta->entity->topic = kRawTextTopic;

    ContextQuery query;
    AddToContextQuery(&query, kRawTextTopic, std::move(selector));

    reader_->Subscribe(std::move(query), binding_.NewBinding());
  }

 private:
  // Return a JSON representation of an array of entities.
  std::string GetEntitiesFromText(const std::string& raw_text) {
    const std::regex entity_regex(kEmailRegex);
    const auto entities_begin =
        std::sregex_iterator(raw_text.begin(), raw_text.end(), entity_regex);
    const auto entities_end = std::sregex_iterator();

    rapidjson::Document d;
    rapidjson::Value entities_json(rapidjson::kArrayType);
    for (std::sregex_iterator i = entities_begin; i != entities_end; ++i) {
      const std::smatch match = *i;
      const std::string content = match.str();
      const int start = match.position();
      const int end = start + match.length();
      const EntitySpan entity(content, kEmailType, start, end);
      // TODO(travismart): It would be more efficient to work directly with
      // JSON values here, so we don't have to make multiple copies of strings
      // and parse them. However, strings allow our interface to be independent
      // of choice of JSON library.
      d.Parse(entity.GetJsonString());
      entities_json.PushBack(d, d.GetAllocator());
    }

    return fuchsia::modular::JsonValueToString(entities_json);
  }

  // |ContextListener|
  void OnContextUpdate(ContextUpdate result) override {
    auto r = TakeContextValue(&result, kRawTextTopic);
    if (!r.first || r.second->empty())
      return;
    rapidjson::Document text_doc;
    // TODO(thatguy): This is only taking the first raw_text entry. We should be
    // keeping track of each one, and writing N new context values out for
    // Entities we extracted.
    text_doc.Parse(r.second->at(0).content);
    // TODO(travismart): What to do if there are multiple topics, or if
    // topics_[0] has more than one entry?
    if (!text_doc.HasMember("text") || !text_doc["text"].IsString()) {
      FXL_LOG(ERROR) << "Invalid " << kRawTextTopic
                     << " entry in Context Engine.";
    }
    const std::string raw_text = text_doc["text"].GetString();

    writer_->WriteEntityTopic(kFocalEntitiesTopic,
                              GetEntitiesFromText(raw_text));
  }

  std::unique_ptr<component::ApplicationContext> app_context_;
  ContextReaderPtr reader_;
  ContextWriterPtr writer_;
  fidl::Binding<ContextListener> binding_;
};

}  // namespace modular
}  // namespace fuchsia

int main(int argc, const char** argv) {
  fsl::MessageLoop loop;
  fuchsia::modular::BasicTextListener app;
  loop.Run();
  return 0;
}
