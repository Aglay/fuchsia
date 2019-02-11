// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/inspect/cpp/fidl.h>
#include <lib/fit/defer.h>
#include <lib/inspect/inspect.h>
#include <lib/inspect/testing/inspect.h>
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using inspect::Object;
using testing::AllOf;
using testing::IsEmpty;
using testing::UnorderedElementsAre;
using namespace inspect::testing;

TEST(Inspect, Object) {
  Object obj("test");
  EXPECT_STREQ("test", obj.name());

  auto output = obj.object();
  EXPECT_STREQ("test", output.name.c_str());
  EXPECT_EQ(0u, output.properties->size());
  EXPECT_EQ(0u, output.metrics->size());
}

class ValueWrapper {
 public:
  ValueWrapper(Object obj, int val)
      : object_(std::move(obj)),
        value_(object_.CreateIntMetric("value", val)) {}

  Object& object() { return object_; };

 private:
  Object object_;
  inspect::IntMetric value_;
};

TEST(Inspect, Child) {
  Object root("root");
  {
    // Create a child and check it exists.
    auto obj = root.CreateChild("child");
    EXPECT_STREQ("child", obj.name());
    EXPECT_THAT(*root.children(), UnorderedElementsAre("child"));

    auto obj2 = root.CreateChild("child2");
    EXPECT_THAT(*root.children(), UnorderedElementsAre("child", "child2"));

    // Check assignment removes the old object.
    obj = root.CreateChild("newchild");
    EXPECT_THAT(*root.children(), UnorderedElementsAre("newchild", "child2"));
  }
  // Check that the child is removed when it goes out of scope.
  EXPECT_THAT(*root.children(), IsEmpty());
}

TEST(Inspect, ChildChaining) {
  Object root("root");
  {
    ValueWrapper v(root.CreateChild("child"), 100);
    EXPECT_THAT(*root.children(), UnorderedElementsAre("child"));
    EXPECT_THAT(
        v.object().object(),
        AllOf(MetricList(UnorderedElementsAre(IntMetricIs("value", 100)))));
  }
  // Check that the child is removed when it goes out of scope.
  EXPECT_THAT(*root.children(), IsEmpty());
}

TEST(Inspect, ChildrenCallbacks) {
  Object root("root");
  {
    inspect::ChildrenCallback callback =
        root.CreateChildrenCallback([](component::Object::ObjectVector* out) {
          out->push_back(component::ObjectDir::Make("temp").object());
        });
    EXPECT_THAT(*root.children(), UnorderedElementsAre("temp"));
  }
  // Check that the child is removed when it goes out of scope.
  EXPECT_THAT(*root.children(), IsEmpty());
}

template <typename Type>
void DefaultMetricTest() {
  Type default_metric;
  default_metric.Add(1);
  default_metric.Subtract(1);
  default_metric.Set(1);
}

TEST(Inspect, Metrics) {
  DefaultMetricTest<inspect::IntMetric>();
  DefaultMetricTest<inspect::UIntMetric>();
  DefaultMetricTest<inspect::DoubleMetric>();

  Object root("root");
  {
    // Create a child and check it exists.
    auto metric_int = root.CreateIntMetric("int", -10);
    metric_int.Add(5);
    metric_int.Subtract(4);
    auto metric_uint = root.CreateUIntMetric("uint", 10);
    metric_uint.Add(4);
    metric_uint.Subtract(5);
    auto metric_double = root.CreateDoubleMetric("double", 0.25);
    metric_double.Add(1);
    metric_double.Subtract(0.5);
    EXPECT_THAT(root.object(),
                AllOf(MetricList(UnorderedElementsAre(
                    IntMetricIs("int", -9), UIntMetricIs("uint", 9),
                    DoubleMetricIs("double", 0.75)))));
  }
  // Check that the metrics are removed when they goes out of scope.
  EXPECT_THAT(root.object(), AllOf(MetricList(IsEmpty())));

  {
    // Test that a later metric overwrites an earlier metric with the same name.
    auto metric_int = root.CreateIntMetric("value", -10);
    auto metric_uint = root.CreateUIntMetric("value", 10);
    EXPECT_THAT(
        root.object(),
        AllOf(MetricList(UnorderedElementsAre(UIntMetricIs("value", 10)))));

    // Deleting any of the owners deletes the value.
    metric_int = root.CreateIntMetric("other", 0);
    EXPECT_THAT(
        root.object(),
        AllOf(MetricList(UnorderedElementsAre(IntMetricIs("other", 0)))));

    // Adding to the deleted value does nothing.
    metric_uint.Add(100);
    EXPECT_THAT(
        root.object(),
        AllOf(MetricList(UnorderedElementsAre(IntMetricIs("other", 0)))));

    // Setting the deleted value recreates it.
    // TODO(CF-275): Fix this behavior.
    metric_uint.Set(100);
    EXPECT_THAT(root.object(),
                AllOf(MetricList(UnorderedElementsAre(
                    UIntMetricIs("value", 100), IntMetricIs("other", 0)))));
  }
}

TEST(Inspect, MetricCallbacks) {
  Object root("root");
  bool defer_called = false;
  auto defer = fit::defer([&defer_called] { defer_called = true; });
  {
    int64_t metric_value = -100;
    // Create a child and check it exists.
    auto metric = root.CreateLazyMetric(
        "value",
        [defer = std::move(defer), &metric_value](component::Metric* value) {
          value->SetInt(metric_value++);
        });
    EXPECT_THAT(
        root.object(),
        AllOf(MetricList(UnorderedElementsAre(IntMetricIs("value", -100)))));
    EXPECT_THAT(
        root.object(),
        AllOf(MetricList(UnorderedElementsAre(IntMetricIs("value", -99)))));
    EXPECT_FALSE(defer_called);
  }
  // Check that the callback is removed and destroyed (defer called) when it
  // goes out of scope.
  EXPECT_THAT(root.object(), AllOf(MetricList(IsEmpty())));
  EXPECT_TRUE(defer_called);
}

TEST(Inspect, Properties) {
  Object root("root");
  {
    auto property_string = root.CreateStringProperty("str", "test");
    property_string.Set("valid");
    auto property_vector =
        root.CreateByteVectorProperty("vec", inspect::VectorValue(3, 'a'));
    EXPECT_THAT(
        root.object(),
        AllOf(PropertyList(UnorderedElementsAre(
            StringPropertyIs("str", "valid"),
            ByteVectorPropertyIs("vec", inspect::VectorValue(3, 'a'))))));
  }
  // Check that the properties are removed when they goes out of scope.
  EXPECT_THAT(root.object(), AllOf(PropertyList(IsEmpty())));

  {
    // Test that a later property overwrites an earlier property with the same
    // name.
    auto property_string = root.CreateStringProperty("string", "a");
    auto property_other = root.CreateStringProperty("string", "b");
    EXPECT_THAT(root.object(), AllOf(PropertyList(UnorderedElementsAre(
                                   StringPropertyIs("string", "b")))));

    // Deleting any of the owners deletes the value.
    property_string = root.CreateStringProperty("not_string", "b");
    EXPECT_THAT(root.object(), AllOf(PropertyList(UnorderedElementsAre(
                                   StringPropertyIs("not_string", "b")))));

    // Setting the deleted value recreates it.
    // TODO(CF-275): Fix this behavior.
    property_other.Set("c");
    EXPECT_THAT(root.object(), AllOf(PropertyList(UnorderedElementsAre(
                                   StringPropertyIs("not_string", "b"),
                                   StringPropertyIs("string", "c")))));
  }
}

TEST(Inspect, PropertyCallbacks) {
  Object root("root");
  bool defer_called1 = false, defer_called2 = false;
  auto defer1 = fit::defer([&defer_called1] { defer_called1 = true; });
  auto defer2 = fit::defer([&defer_called2] { defer_called2 = true; });
  {
    std::string val = "1";
    inspect::VectorValue vec(3, 'a');
    // Create a child and check it exists.
    auto property_string = root.CreateLazyStringProperty(
        "string", [defer = std::move(defer1), &val] {
          val.append("2");
          return val;
        });
    auto property_vector = root.CreateLazyByteVectorProperty(
        "vector", [defer = std::move(defer2), &vec] {
          vec.push_back('a');
          return vec;
        });
    EXPECT_THAT(
        root.object(),
        AllOf(PropertyList(UnorderedElementsAre(
            StringPropertyIs("string", "12"),
            ByteVectorPropertyIs("vector", inspect::VectorValue(4, 'a'))))));
    EXPECT_THAT(
        root.object(),
        AllOf(PropertyList(UnorderedElementsAre(
            StringPropertyIs("string", "122"),
            ByteVectorPropertyIs("vector", inspect::VectorValue(5, 'a'))))));
    EXPECT_FALSE(defer_called1);
    EXPECT_FALSE(defer_called2);
  }
  // Check that the callback is removed and destroyed (defer called) when it
  // goes out of scope.
  EXPECT_THAT(*root.object().properties, IsEmpty());
  EXPECT_TRUE(defer_called1);
  EXPECT_TRUE(defer_called2);
}

}  // namespace
