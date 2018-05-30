// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/examples/counter_cpp/store.h"

#include <string>

#include <modular/cpp/fidl.h>

#include "gtest/gtest.h"
#include "lib/fxl/strings/string_view.h"
#include "lib/gtest/test_with_loop.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/mock_base.h"

namespace modular {
namespace {

class LinkMockBase : protected Link, public testing::MockBase {
 public:
  LinkMockBase() = default;

  ~LinkMockBase() override = default;

  void Get(fidl::VectorPtr<fidl::StringPtr> /*path*/,
           GetCallback /*callback*/) override {
    ++counts["Get"];
  }

  void Set(fidl::VectorPtr<fidl::StringPtr> /*path*/,
           fidl::StringPtr /*json*/) override {
    ++counts["Set"];
  }

  void UpdateObject(fidl::VectorPtr<fidl::StringPtr> /*path*/,
                    fidl::StringPtr /*json*/) override {
    ++counts["UpdateObject"];
  }

  void Erase(fidl::VectorPtr<fidl::StringPtr> /*path*/) override {
    ++counts["Erase"];
  }

  void GetEntity(Link::GetEntityCallback callback) override {
    ++counts["GetEntity"];
    callback("");
  }

  void SetEntity(fidl::StringPtr /*entity_reference*/) override {
    ++counts["SetEntity"];
  }

  void Watch(fidl::InterfaceHandle<LinkWatcher> /*watcher_handle*/) override {
    ++counts["Watch"];
  }

  void WatchAll(fidl::InterfaceHandle<LinkWatcher> /*watcher*/) override {
    ++counts["WatchAll"];
  }

  void Sync(SyncCallback /*callback*/) override { ++counts["Sync"]; }
};

class LinkMock : public LinkMockBase {
 public:
  LinkMock() : binding_(this) {}

  void Bind(fidl::InterfaceRequest<Link> request) {
    binding_.Bind(std::move(request));
  }

  void Watch(fidl::InterfaceHandle<LinkWatcher> watcher_handle) override {
    watcher.Bind(std::move(watcher_handle));
    LinkMockBase::Watch(std::move(watcher_handle));
  }

  void UpdateObject(fidl::VectorPtr<fidl::StringPtr> path,
                    fidl::StringPtr json) override {
    LinkMockBase::UpdateObject(std::move(path), json);
  }

  fidl::InterfacePtr<modular::LinkWatcher> watcher;

 private:
  fidl::Binding<Link> binding_;
};

const std::string module_name{"store_unittest"};

TEST(Counter, Constructor_Invalid) {
  modular_example::Counter counter;
  EXPECT_FALSE(counter.is_valid());
}

TEST(Counter, ToDocument_Success) {
  modular_example::Counter counter;
  counter.counter = 3;
  EXPECT_TRUE(counter.is_valid());

  rapidjson::Document doc = counter.ToDocument(module_name);
  std::string json = JsonValueToString(doc);
  EXPECT_EQ(
      "{\"http://schema.domokit.org/counter\":3,"
      "\"http://schema.org/sender\":\"store_unittest\"}",
      json);
}

class StoreTest : public gtest::TestWithLoop {};

TEST_F(StoreTest, Store_ModelChanged) {
  LinkMock link_mock;
  modular::LinkPtr link;
  link_mock.Bind(link.NewRequest());

  modular_example::Store store{module_name};
  store.Initialize(link.Unbind());
  store.counter.sender = module_name;
  store.counter.counter = 3;

  link_mock.ExpectNoOtherCalls();

  store.MarkDirty();
  store.ModelChanged();

  RunLoopUntilIdle();

  // Initialize() calls Watch()
  // and ModelChanged calls UpdateObject()
  link_mock.ExpectCalledOnce("Watch");
  link_mock.ExpectCalledOnce("UpdateObject");
  link_mock.ExpectNoOtherCalls();
}

}  // namespace
}  // namespace modular
