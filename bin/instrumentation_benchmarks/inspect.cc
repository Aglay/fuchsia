// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <random>
#include <vector>

#include <fbl/ref_ptr.h>
#include <fbl/string_printf.h>
#include <lib/fxl/strings/string_printf.h>
#include <perftest/perftest.h>
#include <zircon/syscalls.h>

#include <iostream>
#include <sstream>

#include "lib/component/cpp/exposed_object.h"

namespace {

using ByteVector = component::Property::ByteVector;
using component::ObjectPath;

const char kValue[] = "value";
const int kSmallPropertySize = 8;
const int kLargePropertySize = 10000;
const ObjectPath kPath0 = {};
const ObjectPath kPath1 = {"a"};
const ObjectPath kPath2 = {"a", "b"};
const ObjectPath kPath10 = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};

class NumericItem : public component::ExposedObject {
 public:
  NumericItem(ObjectPath path) : ExposedObject(UniqueName("itemN-")), path_{std::move(path)} {
    object_dir().set_metric(path_, kValue, component::IntMetric(0));
  }
  NumericItem() : NumericItem(ObjectPath()) {}

  void increment() {
    object_dir().add_metric(path_, kValue, 1);
  }
 private:
  ObjectPath path_;
};

class PropertyItem : public component::ExposedObject {
 public:
  PropertyItem() : ExposedObject(UniqueName("itemS-")) {
    object_dir().set_prop(kValue, component::Property());
  }
  void set(std::string str_value) { object_dir().set_prop(kValue, std::move(str_value)); }
  void set(ByteVector vector_value) { object_dir().set_prop(kValue, std::move(vector_value)); }
};

// Measure the time taken to create and destroy metrics and properties.
bool TestExposedObjectLifecycle(perftest::RepeatState* state) {
  state->DeclareStep("MetricCreate");
  state->DeclareStep("MetricDestroy");
  state->DeclareStep("PropertyCreate");
  state->DeclareStep("PropertyDestroy");
  while (state->KeepRunning()) {
    {
      NumericItem item;
      state->NextStep();
    }
    state->NextStep();
    {
      PropertyItem item;
      state->NextStep();
    }
  }
  return true;
}

// Measure the time taken to increment an IntMetric.
bool TestExposedObjectIncrement(perftest::RepeatState* state) {
  NumericItem item;
  while (state->KeepRunning()) {
    item.increment();
  }
  return true;
}

// Measure the time taken to increment an IntMetric, given a path.
bool TestIncrementPath(perftest::RepeatState* state, ObjectPath path) {
  NumericItem item(path);
  while (state->KeepRunning()) {
    item.increment();
  }
  return true;
}

// Measure the time taken to change a String property.
bool TestExposedObjectSetString(perftest::RepeatState* state, int size) {
  PropertyItem item;
  std::string string;
  string.resize(size, 'a');
  while (state->KeepRunning()) {
    item.set(string);
  }
  return true;
}

// Measure the time taken to change a ByteVector property.
bool TestExposedObjectSetVector(perftest::RepeatState* state, int size) {
  PropertyItem item;
  ByteVector vector;
  vector.resize(size, 'a');
  while (state->KeepRunning()) {
    item.set(vector);
  }
  return true;
}

bool TestExposedObjectParenting(perftest::RepeatState* state) {
  NumericItem parent;
  NumericItem child1;
  NumericItem child2;
  NumericItem child3;
  state->DeclareStep("AddFirst");
  state->DeclareStep("AddSecond");
  state->DeclareStep("AddFirstAgain");
  state->DeclareStep("AddThird");
  state->DeclareStep("RemoveFirst");
  state->DeclareStep("RemoveSecond");
  state->DeclareStep("RemoveFirstAgain");
  state->DeclareStep("RemoveThird");
  while (state->KeepRunning()) {
    child1.set_parent(parent.object_dir());
    state->NextStep();
    child2.set_parent(parent.object_dir());
    state->NextStep();
    child1.set_parent(parent.object_dir());
    state->NextStep();
    child3.set_parent(parent.object_dir());
    state->NextStep();
    child1.remove_from_parent();
    state->NextStep();
    child2.remove_from_parent();
    state->NextStep();
    child1.remove_from_parent();
    state->NextStep();
    child3.remove_from_parent();
  }
  return true;
}

// /pkgfs/packages/instrumentation_benchmarks/0/test/instrumentation_benchmarks -p

void RegisterTests() {
  perftest::RegisterTest("Expose/ExposedObject/Lifecycle", TestExposedObjectLifecycle);
  perftest::RegisterTest("Expose/ExposedObject/Increment", TestExposedObjectIncrement);
  perftest::RegisterTest("Expose/ExposedObject/Parenting", TestExposedObjectParenting);
  perftest::RegisterTest("Expose/ExposedObject/Path/0", TestIncrementPath, kPath0);
  perftest::RegisterTest("Expose/ExposedObject/Path/1", TestIncrementPath, kPath1);
  perftest::RegisterTest("Expose/ExposedObject/Path/2", TestIncrementPath, kPath2);
  perftest::RegisterTest("Expose/ExposedObject/Path/10", TestIncrementPath, kPath10);
  perftest::RegisterTest(fxl::StringPrintf("Expose/ExposedObject/SetString/%d",
                                           kSmallPropertySize).c_str(),
                         TestExposedObjectSetString, kSmallPropertySize);
  perftest::RegisterTest(fxl::StringPrintf("Expose/ExposedObject/SetString/%d",
                                           kLargePropertySize).c_str(),
                         TestExposedObjectSetString, kLargePropertySize);
  perftest::RegisterTest(fxl::StringPrintf("Expose/ExposedObject/SetVector/%d",
                                           kSmallPropertySize).c_str(),
                         TestExposedObjectSetVector, kSmallPropertySize);
  perftest::RegisterTest(fxl::StringPrintf("Expose/ExposedObject/SetVector/%d",
                                           kLargePropertySize).c_str(),
                         TestExposedObjectSetVector, kLargePropertySize);
}
PERFTEST_CTOR(RegisterTests);

}  // namespace
