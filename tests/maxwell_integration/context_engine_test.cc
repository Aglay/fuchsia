// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/context/cpp/context_helper.h"
#include "lib/context/cpp/context_metadata_builder.h"
#include "lib/context/cpp/formatting.h"
#include "lib/context/fidl/context_engine.fidl.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "peridot/bin/context_engine/scope_utils.h"
#include "peridot/tests/maxwell_integration/context_engine_test_base.h"

namespace maxwell {
namespace {

ComponentScopePtr MakeGlobalScope() {
  auto scope = ComponentScope::New();
  scope->set_global_scope(GlobalScope::New());
  return scope;
}

/*
ComponentScopePtr MakeModuleScope(const std::string& path,
                                  const std::string& story_id) {
  auto scope = ComponentScope::New();
  auto module_scope = ModuleScope::New();
  module_scope->url = path;
  module_scope->story_id = story_id;
  module_scope->module_path = f1dl::Array<f1dl::String>::New(1);
  module_scope->module_path[0] = path;
  scope->set_module_scope(std::move(module_scope));
  return scope;
}
*/

class TestListener : public ContextListener {
 public:
  ContextUpdatePtr last_update;

  TestListener() : binding_(this) {}

  void OnContextUpdate(ContextUpdatePtr update) override {
    FXL_LOG(INFO) << "OnUpdate(" << update << ")";
    last_update = std::move(update);
  }

  f1dl::InterfaceHandle<ContextListener> GetHandle() {
    return binding_.NewBinding();
  }

  void Reset() { last_update.reset(); }

 private:
  f1dl::Binding<ContextListener> binding_;
};

class ContextEngineTest : public ContextEngineTestBase {
 public:
  void SetUp() override {
    ContextEngineTestBase::SetUp();
    InitAllGlobalScope();
  }

 protected:
  void InitAllGlobalScope() {
    InitReader(MakeGlobalScope());
    InitWriter(MakeGlobalScope());
  }

  void InitReader(ComponentScopePtr scope) {
    reader_.Unbind();
    context_engine()->GetReader(std::move(scope), reader_.NewRequest());
  }

  void InitWriter(ComponentScopePtr client_info) {
    writer_.Unbind();
    context_engine()->GetWriter(std::move(client_info), writer_.NewRequest());
  }

  ContextReaderPtr reader_;
  ContextWriterPtr writer_;
};

}  // namespace

TEST_F(ContextEngineTest, ContextValueWriter) {
  // Use the ContextValueWriter interface, available by calling
  // ContextWriter.CreateValue().
  ContextValueWriterPtr value1;
  writer_->CreateValue(value1.NewRequest(), ContextValueType::ENTITY);
  value1->Set(R"({ "@type": "someType", "foo": "bar" })",
              ContextMetadataBuilder().SetEntityTopic("topic").Build());

  ContextValueWriterPtr value2;
  writer_->CreateValue(value2.NewRequest(), ContextValueType::ENTITY);
  value2->Set(R"({ "@type": ["someType", "alsoAnotherType"], "baz": "bang" })",
              ContextMetadataBuilder().SetEntityTopic("frob").Build());

  ContextValueWriterPtr value3;
  writer_->CreateValue(value3.NewRequest(), ContextValueType::ENTITY);
  value3->Set(
      entity_resolver().AddEntity({{"someType", ""}, {"evenMoreType", ""}}),
      ContextMetadataBuilder().SetEntityTopic("borf").Build());

  // Subscribe to those values.
  auto selector = ContextSelector::New();
  selector->type = ContextValueType::ENTITY;
  selector->meta = ContextMetadataBuilder().AddEntityType("someType").Build();
  auto query = ContextQuery::New();
  AddToContextQuery(query.get(), "a", std::move(selector));

  TestListener listener;
  f1dl::Array<ContextValuePtr> result;
  reader_->Subscribe(std::move(query), listener.GetHandle());
  ASSERT_TRUE(RunLoopUntil([&listener, &result] {
    return listener.last_update &&
           (result = std::move(TakeContextValue(listener.last_update.get(), "a").second)).size() == 3;
  }));

  EXPECT_EQ("topic", result[0]->meta->entity->topic);
  EXPECT_EQ("frob", result[1]->meta->entity->topic);
  EXPECT_EQ("borf", result[2]->meta->entity->topic);

  // Update value1 and value3 so they no longer matches for the 'someType'
  // query.
  listener.Reset();
  value1->Set(R"({ "@type": "notSomeType", "foo": "bar" })", nullptr);
  value3.Unbind();
  ASSERT_TRUE(RunLoopUntil([&listener, &result] {
    return !!listener.last_update &&
           (result = std::move(TakeContextValue(listener.last_update.get(), "a").second)).size() == 1;
  }));

  EXPECT_EQ("frob", result[0]->meta->entity->topic);

  // Create two new values: A Story value and a child Entity value, where the
  // Entity value matches our query.
  listener.Reset();
  ContextValueWriterPtr story_value;
  writer_->CreateValue(story_value.NewRequest(), ContextValueType::STORY);
  story_value->Set(nullptr,
                   ContextMetadataBuilder().SetStoryId("story").Build());

  ContextValueWriterPtr value4;
  story_value->CreateChildValue(value4.NewRequest(), ContextValueType::ENTITY);
  value4->Set("1", ContextMetadataBuilder().AddEntityType("someType").Build());

  ASSERT_TRUE(RunLoopUntil([&listener] { return !!listener.last_update; }));
  result = std::move(TakeContextValue(listener.last_update.get(), "a").second);
  ASSERT_EQ(2lu, result.size());
  EXPECT_EQ("frob", result[0]->meta->entity->topic);
  EXPECT_EQ("1", result[1]->content);
  EXPECT_EQ("story", result[1]->meta->story->id);

  // Lastly remove one of the values by resetting the ContextValueWriter proxy.
  listener.Reset();
  value4.Unbind();
  RunLoopUntil([&listener] { return !!listener.last_update; });
  result = std::move(TakeContextValue(listener.last_update.get(), "a").second);
  EXPECT_EQ(1lu, result.size());
  EXPECT_EQ("frob", result[0]->meta->entity->topic);
}

TEST_F(ContextEngineTest, CloseListenerAndReader) {
  // Ensure that listeners can be closed individually, and that the reader
  // itself can be closed and listeners are still valid.
  auto selector = ContextSelector::New();
  selector->type = ContextValueType::ENTITY;
  selector->meta = ContextMetadataBuilder().SetEntityTopic("topic").Build();
  auto query = ContextQuery::New();
  AddToContextQuery(query.get(), "a", std::move(selector));

  TestListener listener2;
  {
    TestListener listener1;
    reader_->Subscribe(query.Clone(), listener1.GetHandle());
    reader_->Subscribe(query.Clone(), listener2.GetHandle());
    InitReader(MakeGlobalScope());
    ASSERT_FALSE(
        RunLoopUntil([&listener2] { return !!listener2.last_update; }));
    listener2.Reset();
  }

  // We don't want to crash. There's no way to assert that here, but it will
  // show up in the logs.
  ContextValueWriterPtr value;
  writer_->CreateValue(value.NewRequest(), ContextValueType::ENTITY);
  value->Set(nullptr /* content */,
             ContextMetadataBuilder().SetEntityTopic("topic").Build());
  ASSERT_FALSE(RunLoopUntil([&listener2] { return !!listener2.last_update; }));
}

}  // namespace maxwell
