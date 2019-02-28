// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranking_features/mod_pair_ranking_feature.h"

#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/files/scoped_temp_dir.h"

#include "gtest/gtest.h"

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
  fuchsia::modular::UserInput query;

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
    fidl::VectorPtr<fuchsia::modular::ContextValue>& context_update,
    const std::string& mod) {
  fuchsia::modular::ContextValue value;
  value.meta.mod = fuchsia::modular::ModuleMetadata::New();
  value.meta.mod->url = mod;
  context_update.push_back(std::move(value));
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureAddModuleAction) {
  fuchsia::modular::Intent intent;
  intent.handler = "mod4";
  fuchsia::modular::AddMod add_mod;
  add_mod.intent = std::move(intent);
  fuchsia::modular::StoryCommand command;
  command.set_add_mod(std::move(add_mod));
  fuchsia::modular::Proposal proposal;
  proposal.on_selected.push_back(std::move(command));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype = &prototype;

  fidl::VectorPtr<fuchsia::modular::ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod3");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, 0.8);
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureNoModule) {
  fuchsia::modular::Intent intent;
  intent.handler = "mod-fiction";
  fuchsia::modular::AddMod add_mod;
  add_mod.intent = std::move(intent);
  fuchsia::modular::StoryCommand command;
  command.set_add_mod(std::move(add_mod));
  fuchsia::modular::Proposal proposal;
  proposal.on_selected.push_back(std::move(command));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype = &prototype;

  fidl::VectorPtr<fuchsia::modular::ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod1");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, kMinConfidence);
}

TEST_F(ModPairRankingFeatureTest, ComputeFeatureMultipleActions) {
  fuchsia::modular::AddMod add_mod;
  add_mod.intent.handler = "mod-fiction";
  fuchsia::modular::StoryCommand command;
  command.set_add_mod(std::move(add_mod));
  fuchsia::modular::Proposal proposal;
  proposal.on_selected.push_back(std::move(command));
  SuggestionPrototype prototype;
  prototype.proposal = std::move(proposal);
  RankedSuggestion suggestion;
  suggestion.prototype = &prototype;

  add_mod = fuchsia::modular::AddMod();
  add_mod.intent.handler = "mod3";
  fuchsia::modular::StoryCommand command2;
  command2.set_add_mod(std::move(add_mod));
  suggestion.prototype->proposal.on_selected.push_back(std::move(command2));

  fidl::VectorPtr<fuchsia::modular::ContextValue> context_update;
  AddValueToContextUpdate(context_update, "mod1");
  AddValueToContextUpdate(context_update, "mod2");
  mod_pair_feature.UpdateContext(std::move(context_update));
  double value = mod_pair_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, 1.0);
}

TEST_F(ModPairRankingFeatureTest, CreateContextSelector) {
  auto selector = mod_pair_feature.CreateContextSelector();
  EXPECT_NE(selector, nullptr);
  EXPECT_EQ(selector->type, fuchsia::modular::ContextValueType::MODULE);
  EXPECT_EQ(selector->meta->story->focused->state,
            fuchsia::modular::FocusedStateState::FOCUSED);
}

}  // namespace
}  // namespace modular
