// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranking_features/mod_pair_ranking_feature.h"
#include "gtest/gtest.h"
#include "lib/fxl/files/file.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/files/path.h"
#include "lib/fxl/logging.h"

namespace modular {
namespace {

constexpr char kTestData[] = R"({
  "mod1": {
    "mod2": 0.5,
    "mod3": 0.5
  },
  "mod2": {
    "mod3": 1.0
  },
  "mod3": {
    "mod1": 0.2,
    "mod4": 0.8
  }
})";


class ModPairRankingFeatureTest : public ::testing::Test {
 public:
  void SetUp() override {
    std::string tmp_file;
    ASSERT_TRUE(CreateFile(kTestData, &tmp_file));
    mod_pair_feature.LoadDataFromFile(tmp_file);
  }

 protected:
  ModPairRankingFeature mod_pair_feature{false};
  UserInput query;

 private:
  bool CreateFile(const std::string& content, std::string* const tmp_file) {
    if (!tmp_dir_.NewTempFile(tmp_file)) {
      return false;
    }
    return files::WriteFile(*tmp_file, content.c_str(), content.size());
  }

  files::ScopedTempDir tmp_dir_;
};

// Creates the values from a context query to mock the modules in a focused
// story based on which this ranking feature computes its value.
void AddValueToContextUpdate(
    fidl::VectorPtr<ContextValue>& context_update, const std::string& mod) {
  ContextValue value;
  value.meta.mod = ModuleMetadata::New();
  value.meta.mod->url = mod;
  context_update.push_back(std::move(value));
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureCreateStoryAction)  {
  CreateStory create_story;
  create_story.module_id = "mod3";
  Action action;
  action.set_create_story(std::move(create_story));
  Proposal proposal;
  proposal.on_selected.push_back(std::move(action));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype =  &prototype;

  fidl::VectorPtr<ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod1");
  AddValueToContextUpdate(context_update, "mod2");
  FXL_LOG(INFO) << "SIZE " << context_update->size();
  mod_pair_feature.UpdateContext(context_update);
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, 1.0);
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureAddModuleToStoryAction)  {
  AddModuleToStory add_module_to_story;
  add_module_to_story.module_url = "mod3";
  Action action;
  action.set_add_module_to_story(std::move(add_module_to_story));
  Proposal proposal;
  proposal.on_selected.push_back(std::move(action));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype =  &prototype;

  fidl::VectorPtr<ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod1");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, 0.5);
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureAddModuleAction)  {
  Daisy daisy;
  daisy.url = "mod4";
  AddModule add_module;
  add_module.daisy = std::move(daisy);
  Action action;
  action.set_add_module(std::move(add_module));
  Proposal proposal;
  proposal.on_selected.push_back(std::move(action));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype =  &prototype;

  fidl::VectorPtr<ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod3");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, 0.8);
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureNoModule) {
  Daisy daisy;
  daisy.url = "mod-fiction";
  AddModule add_module;
  add_module.daisy = std::move(daisy);
  Action action;
  action.set_add_module(std::move(add_module));
  Proposal proposal;
  proposal.on_selected.push_back(std::move(action));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype =  &prototype;

  fidl::VectorPtr<ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod1");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, kMinConfidence);
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureMultipleActions) {
  Daisy daisy;
  daisy.url = "mod-fiction";
  AddModule add_module;
  add_module.daisy = std::move(daisy);
  Action action;
  action.set_add_module(std::move(add_module));
  Proposal proposal;
  proposal.on_selected.push_back(std::move(action));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype =  &prototype;

  AddModuleToStory add_module_to_story;
  add_module_to_story.module_url = "mod3";
  Action action2;
  action2.set_add_module_to_story(std::move(add_module_to_story));
  suggestion.prototype->proposal.on_selected.push_back(std::move(action2));

  fidl::VectorPtr<ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod1");
  AddValueToContextUpdate(context_update, "mod2");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, 1.0);
}

TEST_F(ModPairRankingFeatureTest, CreateContextSelector) {
  auto selector = mod_pair_feature.CreateContextSelector();
  EXPECT_NE(selector, nullptr);
  EXPECT_EQ(selector->type, ContextValueType::MODULE);
  EXPECT_EQ(selector->meta->story->focused->state,
            FocusedStateState::FOCUSED);
}

}  // namespace
}  // namespace modular
