// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fs/synchronous-vfs.h>
#include <fuchsia/inspect/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async_promise/executor.h>
#include <lib/fit/bridge.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/inspect/inspect.h>
#include <lib/inspect/reader.h>
#include <lib/inspect/testing/inspect.h>
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <thread>

namespace {

using inspect::Object;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

using namespace inspect::testing;

const char kObjectsName[] = "objects";

class TestReader : public gtest::RealLoopFixture {
 public:
  TestReader()
      : object_(component::Object::Make(kObjectsName)),
        root_object_(component::ObjectDir(object_)),
        executor_(dispatcher()),
        server_loop_(&kAsyncLoopConfigNoAttachToThread) {
    fuchsia::inspect::InspectSyncPtr ptr;
    zx::channel server_channel = ptr.NewRequest().TakeChannel();
    server_thread_ = std::thread([this, server_channel = std::move(
                                            server_channel)]() mutable {
      async_set_default_dispatcher(server_loop_.dispatcher());
      fidl::Binding<fuchsia::inspect::Inspect> binding(
          object_.get(), std::move(server_channel), server_loop_.dispatcher());

      server_loop_.Run();
    });
    client_ = ptr.Unbind();
  }

  ~TestReader() override {
    server_loop_.Quit();
    server_thread_.join();
  }

  void SchedulePromise(fit::pending_task promise) {
    executor_.schedule_task(std::move(promise));
  }

 protected:
  std::shared_ptr<component::Object> object_;
  inspect::Object root_object_;
  fidl::InterfaceHandle<fuchsia::inspect::Inspect> client_;

 private:
  async::Executor executor_;
  std::thread server_thread_;
  async::Loop server_loop_;
};

TEST_F(TestReader, Empty) {
  inspect::ObjectReader reader(std::move(client_));

  fit::result<fuchsia::inspect::Object> result;
  SchedulePromise(
      reader.Read().then([&](fit::result<fuchsia::inspect::Object>& res) {
        result = std::move(res);
      }));

  ASSERT_TRUE(RunLoopUntil([&] { return !!result; }));
  EXPECT_THAT(result.value(),
              AllOf(NameMatches(kObjectsName), MetricList(IsEmpty()),
                    PropertyList(IsEmpty())));
}

TEST_F(TestReader, Values) {
  auto metric_int = root_object_.CreateIntMetric("int", -10);
  auto metric_uint = root_object_.CreateUIntMetric("uint", 10);
  auto metric_double = root_object_.CreateDoubleMetric("double", 1.25);
  auto prop_string = root_object_.CreateStringProperty("string", "value");
  auto prop_bytes = root_object_.CreateByteVectorProperty(
      "bytes", inspect::VectorValue(3, 'a'));

  inspect::ObjectReader reader(std::move(client_));
  fit::result<fuchsia::inspect::Object> result;
  SchedulePromise(
      reader.Read().then([&](fit::result<fuchsia::inspect::Object>& res) {
        result = std::move(res);
      }));

  ASSERT_TRUE(RunLoopUntil([&] { return !!result; }));

  EXPECT_THAT(
      result.take_value(),
      AllOf(NameMatches(kObjectsName),
            PropertyList(UnorderedElementsAre(
                StringPropertyIs("string", "value"),
                ByteVectorPropertyIs("bytes", inspect::VectorValue(3, 'a')))),
            MetricList(UnorderedElementsAre(IntMetricIs("int", -10),
                                            UIntMetricIs("uint", 10),
                                            DoubleMetricIs("double", 1.25)))));
}

TEST_F(TestReader, ListChildren) {
  auto child_a = root_object_.CreateChild("child a");
  auto child_b = root_object_.CreateChild("child b");

  inspect::ObjectReader reader(std::move(client_));
  fit::result<inspect::ChildNameVector> result;
  SchedulePromise(reader.ListChildren().then(
      [&](fit::result<inspect::ChildNameVector>& res) {
        result = std::move(res);
      }));

  ASSERT_TRUE(RunLoopUntil([&] { return !!result; }));

  auto children = result.take_value();
  EXPECT_THAT(*children, UnorderedElementsAre("child a", "child b"));
}

TEST_F(TestReader, OpenChild) {
  auto child_a = root_object_.CreateChild("child a");
  auto metric_a = child_a.CreateIntMetric("value", 1);
  auto child_b = root_object_.CreateChild("child b");

  inspect::ObjectReader reader(std::move(client_));
  fit::result<fuchsia::inspect::Object> result;
  std::vector<std::string> child_prefix_path;
  SchedulePromise(
      reader.OpenChild("child a")
          .and_then([&child_prefix_path](inspect::ObjectReader& child_reader) {
            child_prefix_path = child_reader.GetPrefixPath();
            return child_reader.Read();
          })
          .then([&](fit::result<fuchsia::inspect::Object>& res) {
            result = std::move(res);
          }));

  ASSERT_TRUE(RunLoopUntil([&] { return !!result; }));

  auto child_value = result.take_value();
  EXPECT_THAT(child_value,
              AllOf(NameMatches("child a"),
                    MetricList(UnorderedElementsAre(IntMetricIs("value", 1)))));
  EXPECT_THAT(child_prefix_path, testing::ElementsAre("child a"));
}

TEST_F(TestReader, OpenChildren) {
  auto child_a = root_object_.CreateChild("child a");
  auto metric_a = child_a.CreateIntMetric("value", 1);
  auto child_b = root_object_.CreateChild("child b");
  auto metric_b = child_b.CreateIntMetric("value", 1);

  inspect::ObjectReader reader(std::move(client_));
  std::vector<fit::result<fuchsia::inspect::Object>> result;
  std::vector<std::string> child_paths;
  SchedulePromise(
      reader.OpenChildren()
          .and_then(
              [&child_paths](std::vector<inspect::ObjectReader>& child_reader) {
                std::vector<fit::promise<fuchsia::inspect::Object>> promises;

                for (auto& child : child_reader) {
                  child_paths.push_back(child.GetPrefixPath().at(0));
                  promises.emplace_back(child.Read());
                }

                return fit::join_promise_vector(std::move(promises));
              })
          .and_then(
              [&](std::vector<fit::result<fuchsia::inspect::Object>>& res) {
                for (auto& r : res) {
                  result.emplace_back(std::move(r));
                }
              }));

  ASSERT_TRUE(RunLoopUntil([&] { return result.size() == 2; }));

  for (size_t i = 0; i < result.size(); i++) {
    ASSERT_TRUE(result[i].is_ok());
    EXPECT_STRNE("", child_paths[i].c_str());
    EXPECT_THAT(
        result[i].take_value(),
        AllOf(NameMatches(child_paths[i].c_str()),
              MetricList(UnorderedElementsAre(IntMetricIs("value", 1)))));
  }
}

// Construct and expect this hierarchy for the following tests:
//
// objects:
//   child a:
//     value = 1
//   child b:
//     value = 2u
//     child c:
//       value = 3f
class TestHierarchy : public TestReader {
 public:
  TestHierarchy() {
    child_a_ = root_object_.CreateChild("child a");
    metric_a_ = child_a_.CreateIntMetric("value", 1);
    child_b_ = root_object_.CreateChild("child b");
    metric_b_ = child_b_.CreateUIntMetric("value", 2);
    child_b_c_ = child_b_.CreateChild("child c");
    metric_c_ = child_b_c_.CreateDoubleMetric("value", 3);
  }

  void ExpectHierarchy(const inspect::ObjectHierarchy& hierarchy) {
    EXPECT_THAT(hierarchy.object(), AllOf(NameMatches(kObjectsName)));
    EXPECT_THAT(hierarchy.GetPrefixPath(), IsEmpty());
    EXPECT_THAT(
        hierarchy.children(),
        UnorderedElementsAre(
            AllOf(ObjectMatches(AllOf(NameMatches("child a"),
                                      MetricList(UnorderedElementsAre(
                                          IntMetricIs("value", 1))))),
                  PrefixPathMatches(ElementsAre("child a")),
                  ChildrenMatch(IsEmpty())),
            AllOf(ObjectMatches(AllOf(NameMatches("child b"),
                                      MetricList(UnorderedElementsAre(
                                          UIntMetricIs("value", 2))))),
                  PrefixPathMatches(ElementsAre("child b")),
                  ChildrenMatch(UnorderedElementsAre(AllOf(
                      ObjectMatches(AllOf(NameMatches("child c"),
                                          MetricList(UnorderedElementsAre(
                                              DoubleMetricIs("value", 3))))),
                      PrefixPathMatches(ElementsAre("child b", "child c")),
                      ChildrenMatch(IsEmpty())))))));
    auto* hierarchy_c = hierarchy.GetByPath({"child b", "child c"});
    ASSERT_THAT(hierarchy_c, ::testing::NotNull());
    EXPECT_THAT(
        hierarchy_c->MakeFormattedPath("/hub/r/sys/1000/c/test/100/"),
        ::testing::StrEq("/hub/r/sys/1000/c/test/100/child b/child c"));
  };

 private:
  inspect::Object child_a_, child_b_, child_b_c_;
  inspect::IntMetric metric_a_;
  inspect::UIntMetric metric_b_;
  inspect::DoubleMetric metric_c_;
};

TEST_F(TestHierarchy, ObjectHierarchy) {
  fit::result<inspect::ObjectHierarchy> result;
  SchedulePromise(
      inspect::ObjectHierarchy::Make(inspect::ObjectReader(std::move(client_)))
          .then([&](fit::result<inspect::ObjectHierarchy>& res) {
            result = std::move(res);
          }));

  ASSERT_TRUE(RunLoopUntil([&] { return !!result; }));

  auto hierarchy = result.take_value();

  ExpectHierarchy(hierarchy);
}

TEST_F(TestHierarchy, ObjectHierarchyLimitDepth) {
  fit::result<inspect::ObjectHierarchy> result;
  SchedulePromise(inspect::ObjectHierarchy::Make(
                      inspect::ObjectReader(std::move(client_)), /*depth=*/1)
                      .then([&](fit::result<inspect::ObjectHierarchy>& res) {
                        result = std::move(res);
                      }));

  ASSERT_TRUE(RunLoopUntil([&] { return !!result; }));

  auto hierarchy = result.take_value();

  EXPECT_THAT(hierarchy, ChildrenMatch(UnorderedElementsAre(
                             ObjectMatches(AllOf(NameMatches("child a"))),
                             ObjectMatches(AllOf(NameMatches("child b"))))));

  auto* hierarchy_b = hierarchy.GetByPath({"child b"});
  ASSERT_THAT(hierarchy_b, ::testing::NotNull());
  EXPECT_THAT(*hierarchy_b, ChildrenMatch(IsEmpty()));
}

TEST_F(TestHierarchy, ObjectHierarchyDirect) {
  auto hierarchy = inspect::ObjectHierarchy::Make(root_object_);

  ExpectHierarchy(hierarchy);
}

TEST_F(TestHierarchy, ObjectHierarchyDirectLimitDepth) {
  auto hierarchy = inspect::ObjectHierarchy::Make(root_object_, /*depth=*/1);

  EXPECT_THAT(hierarchy, ChildrenMatch(UnorderedElementsAre(
                             ObjectMatches(AllOf(NameMatches("child a"))),
                             ObjectMatches(AllOf(NameMatches("child b"))))));

  auto* hierarchy_b = hierarchy.GetByPath({"child b"});
  ASSERT_THAT(hierarchy_b, ::testing::NotNull());
  EXPECT_THAT(*hierarchy_b, ChildrenMatch(IsEmpty()));
}

}  // namespace
