// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <sstream>
#include <string>

#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/application_context.h"
#include "lib/context/cpp/context_helper.h"
#include "lib/context/cpp/formatting.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

namespace {

constexpr char kModuleUrl[] =
    "file:///system/test/modular_tests/context_link_test_module";
constexpr char kTopic[] = "link/context_link_test";
constexpr char kLink[] = "context_link";

// A context reader watcher implementation.
class ContextListenerImpl : modular::ContextListener {
 public:
  ContextListenerImpl() : binding_(this) {
    handler_ = [](const modular::ContextValue&) {};
  }

  ~ContextListenerImpl() override = default;

  // Registers itself a watcher on the given story provider. Only one story
  // provider can be watched at a time.
  void Listen(modular::ContextReader* const context_reader) {
    // Subscribe to all entity values.
    modular::ContextSelector selector;
    selector.type = modular::ContextValueType::ENTITY;

    modular::ContextQuery query;
    AddToContextQuery(&query, "all", std::move(selector));

    context_reader->Subscribe(std::move(query), binding_.NewBinding());
    binding_.set_error_handler([] {
      FXL_LOG(ERROR) << "Lost ContextListener connection to ContextReader.";
    });
  }

  using Handler = std::function<void(const modular::ContextValue&)>;

  void Handle(const Handler& handler) { handler_ = handler; }

  // Deregisters itself from the watched story provider.
  void Reset() { binding_.Unbind(); }

 private:
  // |ContextListener|
  void OnContextUpdate(modular::ContextUpdate update) override {
    FXL_VLOG(4) << "ContextListenerImpl::OnUpdate()";
    auto values = TakeContextValue(&update, "all");
    for (const auto& value : *values.second) {
      FXL_VLOG(4) << "ContextListenerImpl::OnUpdate() " << value;
      handler_(value);
    }
  }

  fidl::Binding<modular::ContextListener> binding_;
  Handler handler_;
  FXL_DISALLOW_COPY_AND_ASSIGN(ContextListenerImpl);
};

// Cf. README.md for what this test does and how.
class TestApp : public modular::testing::ComponentBase<modular::UserShell> {
 public:
  TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  TestPoint initialize_{"Initialize()"};

  // |UserShell|
  void Initialize(fidl::InterfaceHandle<modular::UserShellContext>
                      user_shell_context) override {
    initialize_.Pass();

    user_shell_context_.Bind(std::move(user_shell_context));

    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());

    modular::IntelligenceServicesPtr intelligence_services;
    user_shell_context_->GetIntelligenceServices(
        intelligence_services.NewRequest());
    intelligence_services->GetContextReader(context_reader_.NewRequest());
    context_listener_.Listen(context_reader_.get());
    context_reader_.set_error_handler(
        [] { FXL_LOG(ERROR) << "Lost ContextReader connection."; });

    CreateStory();
  }

  TestPoint create_story_{"CreateStory()"};

  void CreateStory() {
    story_provider_->CreateStory(kModuleUrl, [this](fidl::StringPtr story_id) {
      story_id_ = story_id;
      create_story_.Pass();
      StartStory();
    });
  }

  TestPoint start_story_enter_{"StartStory() Enter"};
  TestPoint start_story_exit_{"StartStory() Exit"};

  void StartStory() {
    start_story_enter_.Pass();

    context_listener_.Handle(
        [this](const modular::ContextValue& value) { GetContextTopic(value); });

    story_provider_->GetController(story_id_, story_controller_.NewRequest());

    // Start and show the new story.
    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());

    start_story_exit_.Pass();
  }

  // NOTE(mesch): We would like to test that changes of value are also picked up
  // by the story_info agent, but we cannot right now guarantee that the
  // story_info agent is already running when the module starts writing to the
  // link, so trying to verify that intermediate context link values are seen by
  // a context watcher proved to be flaky. MI4-780
  TestPoint get_context_topic_{"GetContextTopic() value=2"};
  int get_context_topic_called_{};

  void GetContextTopic(const modular::ContextValue& value) {
    // The context link value has metadata that is derived from the story id
    // in which it was published.
    if (!value.meta.story || !value.meta.entity) {
      FXL_LOG(ERROR) << "ContextValue missing story or entity metadata: "
                     << value;
      return;
    }

    if (value.meta.story->id != story_id_) {
      FXL_LOG(ERROR) << "ContextValue metadata has wrong story id. "
                     << "Expected: " << story_id_ << ". "
                     << "Actual: " << value;
      return;
    }

    if (value.meta.entity->topic != kTopic) {
      FXL_LOG(ERROR) << "ContextValue metadata has wrong topic. "
                     << "Expected: " << kTopic << ". "
                     << "Actual: " << value;
      return;
    }

    FXL_LOG(INFO) << "Context value for topic " << kTopic << " is: " << value;

    modular::JsonDoc doc;
    doc.Parse(value.content);

    if (doc.HasParseError()) {
      FXL_LOG(ERROR) << "JSON Parse Error";
      Logout();
      return;
    }

    if (!doc.IsObject()) {
      FXL_LOG(ERROR) << "JSON not an Object";
      Logout();
      return;
    }

    if (!doc.HasMember("@source")) {
      FXL_LOG(ERROR) << "JSON missing @source";
      Logout();
      return;
    }

    if (!doc["@source"].IsObject()) {
      FXL_LOG(ERROR) << "JSON @source not an Object";
      Logout();
      return;
    }

    if (!doc["@source"].HasMember("link_name")) {
      FXL_LOG(ERROR) << "JSON @source missing link_name";
      Logout();
      return;
    }

    if (!doc["@source"]["link_name"].IsString()) {
      FXL_LOG(ERROR) << "JSON @source link_name not a string";
      Logout();
      return;
    }

    // HACK(mesch): Comparing GetString() to kLink always fails.
    const std::string link_name{doc["@source"]["link_name"].GetString()};
    if (link_name != std::string{kLink}) {
      FXL_LOG(ERROR) << "JSON @source wrong link_name " << link_name;
      Logout();
      return;
    }

    if (!doc.HasMember("link_value")) {
      FXL_LOG(ERROR) << "JSON missing property link_value (set by module)";
      Logout();
      return;
    }

    if (!doc["link_value"].IsString()) {
      FXL_LOG(ERROR) << "JSON link_value (set by module) not a String";
      Logout();
      return;
    }

    const std::string link_value{doc["link_value"].GetString()};
    if (link_value != std::string{"1"} && link_value != std::string{"2"}) {
      FXL_LOG(ERROR) << "JSON link_value (set by module) wrong: " << link_value;
      Logout();
      return;
    }

    if (link_value == std::string{"2"}) {
      if (++get_context_topic_called_ == 1) {
        get_context_topic_.Pass();

        context_listener_.Reset();
        context_listener_.Handle([this](const modular::ContextValue&) {});

        Logout();
      }
    }
  }

  void Logout() { user_shell_context_->Logout(); }

  modular::UserShellContextPtr user_shell_context_;
  modular::StoryProviderPtr story_provider_;

  fidl::StringPtr story_id_;
  modular::StoryControllerPtr story_controller_;

  modular::ContextReaderPtr context_reader_;
  ContextListenerImpl context_listener_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
