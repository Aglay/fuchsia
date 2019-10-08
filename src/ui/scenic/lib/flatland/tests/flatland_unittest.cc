// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/flatland/flatland.h"

#include <gtest/gtest.h>

#include "lib/gtest/test_loop_fixture.h"
#include "src/lib/fxl/logging.h"

using flatland::Flatland;
using LinkId = flatland::Flatland::LinkId;
using fuchsia::ui::scenic::internal::ContentLinkStatus;
using fuchsia::ui::scenic::internal::Flatland_Present_Result;
using FlatlandTestLoop = gtest::TestLoopFixture;
using fuchsia::ui::scenic::internal::ContentLink;
using fuchsia::ui::scenic::internal::ContentLinkToken;
using fuchsia::ui::scenic::internal::GraphLink;
using fuchsia::ui::scenic::internal::GraphLinkToken;
using fuchsia::ui::scenic::internal::LayoutInfo;
using fuchsia::ui::scenic::internal::LinkProperties;

// This is a macro so that, if the various test macros fail, we get a line number associated with a
// particular Present() call in a unit test.
#define PRESENT(flatland, expect_success)                                             \
  {                                                                                   \
    bool processed_callback = false;                                                  \
    flatland.Present([&](Flatland_Present_Result result) {                            \
      ASSERT_EQ(!expect_success, result.is_err());                                    \
      if (expect_success)                                                             \
        EXPECT_EQ(1u, result.response().num_presents_remaining);                      \
      else                                                                            \
        EXPECT_EQ(fuchsia::ui::scenic::internal::Error::BAD_OPERATION, result.err()); \
      processed_callback = true;                                                      \
    });                                                                               \
    EXPECT_TRUE(processed_callback);                                                  \
  }

namespace {
void CreateLink(const std::shared_ptr<Flatland::ObjectLinker>& linker, Flatland* parent,
                Flatland* child, LinkId id, fidl::InterfacePtr<ContentLink>* content_link,
                fidl::InterfacePtr<GraphLink>* graph_link) {
  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  LinkProperties properties;
  parent->CreateLink(id, std::move(parent_token), std::move(properties),
                     content_link->NewRequest());
  child->LinkToParent(std::move(child_token), graph_link->NewRequest());
  PRESENT((*parent), true);
  PRESENT((*child), true);
}
}  // namespace

namespace flatland {
namespace test {

TEST(FlatlandTest, PresentShouldReturnOne) {
  Flatland flatland;
  PRESENT(flatland, true);
}

TEST(FlatlandTest, CreateAndReleaseTransformValidCases) {
  Flatland flatland;

  const uint64_t kId1 = 1;
  const uint64_t kId2 = 2;

  // Create two transforms.
  flatland.CreateTransform(kId1);
  flatland.CreateTransform(kId2);
  PRESENT(flatland, true);

  // Clear, then create two transforms in the other order.
  flatland.ClearGraph();
  flatland.CreateTransform(kId2);
  flatland.CreateTransform(kId1);
  PRESENT(flatland, true);

  // Clear, create and release transforms, non-overlapping.
  flatland.ClearGraph();
  flatland.CreateTransform(kId1);
  flatland.ReleaseTransform(kId1);
  flatland.CreateTransform(kId2);
  flatland.ReleaseTransform(kId2);
  PRESENT(flatland, true);

  // Clear, create and release transforms, nested.
  flatland.ClearGraph();
  flatland.CreateTransform(kId2);
  flatland.CreateTransform(kId1);
  flatland.ReleaseTransform(kId1);
  flatland.ReleaseTransform(kId2);
  PRESENT(flatland, true);

  // Reuse the same id, legally, in a single present call.
  flatland.CreateTransform(kId1);
  flatland.ReleaseTransform(kId1);
  flatland.CreateTransform(kId1);
  flatland.ClearGraph();
  flatland.CreateTransform(kId1);
  PRESENT(flatland, true);

  // Create and clear, overlapping, with multiple present calls.
  flatland.ClearGraph();
  flatland.CreateTransform(kId2);
  PRESENT(flatland, true);
  flatland.CreateTransform(kId1);
  flatland.ReleaseTransform(kId2);
  PRESENT(flatland, true);
  flatland.ReleaseTransform(kId1);
  PRESENT(flatland, true);
}

TEST(FlatlandTest, CreateAndReleaseTransformErrorCases) {
  Flatland flatland;

  const uint64_t kId1 = 1;
  const uint64_t kId2 = 2;

  // Zero is not a valid transform id.
  flatland.CreateTransform(0);
  PRESENT(flatland, false);
  flatland.ReleaseTransform(0);
  PRESENT(flatland, false);

  // Double creation is an error.
  flatland.CreateTransform(kId1);
  flatland.CreateTransform(kId1);
  PRESENT(flatland, false);

  // Releasing a non-existent transform is an error.
  flatland.ReleaseTransform(kId2);
  PRESENT(flatland, false);
}

TEST(FlatlandTest, AddAndRemoveChildValidCases) {
  Flatland flatland;

  const uint64_t kIdParent = 1;
  const uint64_t kIdChild1 = 2;
  const uint64_t kIdChild2 = 3;
  const uint64_t kIdGrandchild = 4;

  flatland.CreateTransform(kIdParent);
  flatland.CreateTransform(kIdChild1);
  flatland.CreateTransform(kIdChild2);
  flatland.CreateTransform(kIdGrandchild);
  PRESENT(flatland, true);

  // Add and remove.
  flatland.AddChild(kIdParent, kIdChild1);
  flatland.RemoveChild(kIdParent, kIdChild1);
  PRESENT(flatland, true);

  // Add two children.
  flatland.AddChild(kIdParent, kIdChild1);
  flatland.AddChild(kIdParent, kIdChild2);
  PRESENT(flatland, true);

  // Remove two children.
  flatland.RemoveChild(kIdParent, kIdChild1);
  flatland.RemoveChild(kIdParent, kIdChild2);
  PRESENT(flatland, true);

  // Add two-deep hierarchy.
  flatland.AddChild(kIdParent, kIdChild1);
  flatland.AddChild(kIdChild1, kIdGrandchild);
  PRESENT(flatland, true);

  // Add sibling.
  flatland.AddChild(kIdParent, kIdChild2);
  PRESENT(flatland, true);

  // Add shared grandchild (deadly diamond dependency).
  flatland.AddChild(kIdChild2, kIdGrandchild);
  PRESENT(flatland, true);

  // Remove original deep-hierarchy.
  flatland.RemoveChild(kIdChild1, kIdGrandchild);
  PRESENT(flatland, true);
}

TEST(FlatlandTest, AddAndRemoveChildErrorCases) {
  Flatland flatland;

  const uint64_t kIdParent = 1;
  const uint64_t kIdChild = 2;
  const uint64_t kIdNotCreated = 3;

  // Setup.
  flatland.CreateTransform(kIdParent);
  flatland.CreateTransform(kIdChild);
  flatland.AddChild(kIdParent, kIdChild);
  PRESENT(flatland, true);

  // Zero is not a valid transform id.
  flatland.AddChild(0, 0);
  PRESENT(flatland, false);
  flatland.AddChild(kIdParent, 0);
  PRESENT(flatland, false);
  flatland.AddChild(0, kIdChild);
  PRESENT(flatland, false);

  // Child does not exist.
  flatland.AddChild(kIdParent, kIdNotCreated);
  PRESENT(flatland, false);
  flatland.RemoveChild(kIdParent, kIdNotCreated);
  PRESENT(flatland, false);

  // Parent does not exist.
  flatland.AddChild(kIdNotCreated, kIdChild);
  PRESENT(flatland, false);
  flatland.RemoveChild(kIdNotCreated, kIdChild);
  PRESENT(flatland, false);

  // Child is already a child of parent.
  flatland.AddChild(kIdParent, kIdChild);
  PRESENT(flatland, false);

  // Both nodes exist, but not in the correct relationship.
  flatland.RemoveChild(kIdChild, kIdParent);
  PRESENT(flatland, false);
}

TEST(FlatlandTest, MultichildUsecase) {
  Flatland flatland;

  const uint64_t kIdParent1 = 1;
  const uint64_t kIdParent2 = 2;
  const uint64_t kIdChild1 = 3;
  const uint64_t kIdChild2 = 4;
  const uint64_t kIdChild3 = 5;

  // Setup
  flatland.CreateTransform(kIdParent1);
  flatland.CreateTransform(kIdParent2);
  flatland.CreateTransform(kIdChild1);
  flatland.CreateTransform(kIdChild2);
  flatland.CreateTransform(kIdChild3);
  PRESENT(flatland, true);

  // Add all children to first parent.
  flatland.AddChild(kIdParent1, kIdChild1);
  flatland.AddChild(kIdParent1, kIdChild2);
  flatland.AddChild(kIdParent1, kIdChild3);
  PRESENT(flatland, true);

  // Add all children to second parent.
  flatland.AddChild(kIdParent2, kIdChild1);
  flatland.AddChild(kIdParent2, kIdChild2);
  flatland.AddChild(kIdParent2, kIdChild3);
  PRESENT(flatland, true);
}

TEST(FlatlandTest, CycleDetector) {
  Flatland flatland;

  const uint64_t kId1 = 1;
  const uint64_t kId2 = 2;
  const uint64_t kId3 = 3;
  const uint64_t kId4 = 4;

  // Create an immediate cycle.
  {
    flatland.CreateTransform(kId1);
    flatland.AddChild(kId1, kId1);
    PRESENT(flatland, false);
  }

  // Create a legal chain of depth one.
  // Then, create a cycle of length 2.
  {
    flatland.ClearGraph();
    flatland.CreateTransform(kId1);
    flatland.CreateTransform(kId2);
    flatland.AddChild(kId1, kId2);
    PRESENT(flatland, true);

    flatland.AddChild(kId2, kId1);
    PRESENT(flatland, false);
  }

  // Create two legal chains of length one.
  // Then, connect each chain into a cycle of length four.
  {
    flatland.ClearGraph();
    flatland.CreateTransform(kId1);
    flatland.CreateTransform(kId2);
    flatland.CreateTransform(kId3);
    flatland.CreateTransform(kId4);
    flatland.AddChild(kId1, kId2);
    flatland.AddChild(kId3, kId4);
    PRESENT(flatland, true);

    flatland.AddChild(kId2, kId3);
    flatland.AddChild(kId4, kId1);
    PRESENT(flatland, false);
  }

  // Create a cycle, where the root is not involved in the cycle.
  {
    flatland.ClearGraph();
    flatland.CreateTransform(kId1);
    flatland.CreateTransform(kId2);
    flatland.CreateTransform(kId3);
    flatland.CreateTransform(kId4);

    flatland.AddChild(kId1, kId2);
    flatland.AddChild(kId2, kId3);
    flatland.AddChild(kId3, kId2);
    flatland.AddChild(kId3, kId4);

    flatland.SetRootTransform(kId1);
    flatland.ReleaseTransform(kId1);
    flatland.ReleaseTransform(kId2);
    flatland.ReleaseTransform(kId3);
    flatland.ReleaseTransform(kId4);
    PRESENT(flatland, false);
  }
}

TEST(FlatlandTest, SetRootTransform) {
  Flatland flatland;

  const uint64_t kId1 = 1;
  const uint64_t kIdNotCreated = 2;

  flatland.CreateTransform(kId1);
  PRESENT(flatland, true);

  // Even with no root transform, so clearing it is not an error.
  flatland.SetRootTransform(0);
  PRESENT(flatland, true);

  // Setting the root to an unknown transform is an error.
  flatland.SetRootTransform(kIdNotCreated);
  PRESENT(flatland, false);

  flatland.SetRootTransform(kId1);
  PRESENT(flatland, true);

  // Releasing the root is allowed.
  flatland.ReleaseTransform(kId1);
  PRESENT(flatland, true);

  // Clearing the root after release is also allowed.
  flatland.SetRootTransform(0);
  PRESENT(flatland, true);

  // Setting the root to a released transform is not allowed.
  flatland.SetRootTransform(kId1);
  PRESENT(flatland, false);
}

TEST_F(FlatlandTestLoop, GraphLinkReplaceWithoutConnection) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();
  Flatland flatland(linker);

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<GraphLink> graph_link;
  flatland.LinkToParent(std::move(child_token), graph_link.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  ContentLinkToken parent_token2;
  GraphLinkToken child_token2;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token2.value, &child_token2.value));

  fidl::InterfacePtr<GraphLink> graph_link2;
  flatland.LinkToParent(std::move(child_token2), graph_link2.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  // TODO(37597): Test for cleanup of previous link here.
}

TEST_F(FlatlandTestLoop, ContentLinkIdIsZero) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();
  Flatland flatland(linker);

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  flatland.CreateLink(0, std::move(parent_token), std::move(properties), content_link.NewRequest());
  RunLoopUntilIdle();
  PRESENT(flatland, false);
  RunLoopUntilIdle();
}

TEST_F(FlatlandTestLoop, ContentLinkIdCollision) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();
  Flatland flatland(linker);

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  flatland.CreateLink(kId1, std::move(parent_token), std::move(properties),
                      content_link.NewRequest());
  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  ContentLinkToken parent_token2;
  GraphLinkToken child_token2;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token2.value, &child_token2.value));

  RunLoopUntilIdle();
  flatland.CreateLink(kId1, std::move(parent_token2), std::move(properties),
                      content_link.NewRequest());
  RunLoopUntilIdle();
  PRESENT(flatland, false);
}

// This code doesn't use the helper function to create a link, because it tests intermediate steps
// and timing corner cases.
TEST_F(FlatlandTestLoop, ValidParentToChildFlow) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();

  Flatland parent(linker);
  Flatland child(linker);

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({1.0f, 2.0f});
  parent.CreateLink(kId1, std::move(parent_token), std::move(properties),
                    content_link.NewRequest());

  fidl::InterfacePtr<GraphLink> graph_link;
  child.LinkToParent(std::move(child_token), graph_link.NewRequest());

  bool layout_updated = false;
  graph_link->GetLayout([&](LayoutInfo info) {
    EXPECT_EQ(1.0f, info.logical_size().x);
    EXPECT_EQ(2.0f, info.logical_size().y);
    layout_updated = true;
  });

  // Layout is updated once the parent has presented and the looper has run. Not before. The child
  // instance has never presented, yet it should still receive events over the link.
  RunLoopUntilIdle();
  PRESENT(parent, true);
  EXPECT_FALSE(layout_updated);
  RunLoopUntilIdle();
  EXPECT_TRUE(layout_updated);
}

// This code doesn't use the helper function to create a link, because it tests intermediate steps
// and timing corner cases.
TEST_F(FlatlandTestLoop, ValidChildToParentFlow) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();

  Flatland parent(linker);
  Flatland child(linker);

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({1.0f, 2.0f});
  parent.CreateLink(kId1, std::move(parent_token), std::move(properties),
                    content_link.NewRequest());

  fidl::InterfacePtr<GraphLink> graph_link;
  child.LinkToParent(std::move(child_token), graph_link.NewRequest());

  bool status_updated = false;
  content_link->GetStatus([&](ContentLinkStatus status) {
    ASSERT_EQ(ContentLinkStatus::CONTENT_HAS_PRESENTED, status);
    status_updated = true;
  });

  // Status is updated once the child has presented and the looper has run. Not before. The parent
  // instance has never presented, yet it should still receive events over the link.
  RunLoopUntilIdle();
  PRESENT(child, true);
  EXPECT_FALSE(status_updated);
  RunLoopUntilIdle();
  EXPECT_TRUE(status_updated);
}

TEST_F(FlatlandTestLoop, SetLinkPropertiesDefaultBehavior) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();

  const uint64_t kLinkId = 1;

  Flatland parent(linker);
  Flatland child(linker);
  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(linker, &parent, &child, kLinkId, &content_link, &graph_link);
  RunLoopUntilIdle();

  const float kDefaultSize = 1.0f;

  // Confirm that the current layout is the default.
  {
    bool layout_updated = false;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kDefaultSize, info.logical_size().x);
      EXPECT_EQ(kDefaultSize, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    RunLoopUntilIdle();
    EXPECT_TRUE(layout_updated);
  }

  // Set the logical size to something new.
  {
    LinkProperties properties;
    properties.set_logical_size({2.0f, 3.0f});
    parent.SetLinkProperties(kLinkId, std::move(properties));
    PRESENT(parent, true);
  }

  // Confirm that the new logical size is accessable.
  {
    bool layout_updated = false;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(2.0f, info.logical_size().x);
      EXPECT_EQ(3.0f, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    RunLoopUntilIdle();
    EXPECT_TRUE(layout_updated);
  }

  // Set the logical size back to the default by using an unset properties object.
  {
    LinkProperties default_properties;
    parent.SetLinkProperties(kLinkId, std::move(default_properties));
    PRESENT(parent, true);
  }

  // Confirm that the current layout is back to the default.
  {
    bool layout_updated = false;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kDefaultSize, info.logical_size().x);
      EXPECT_EQ(kDefaultSize, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    RunLoopUntilIdle();
    EXPECT_TRUE(layout_updated);
  }
}

TEST_F(FlatlandTestLoop, SetLinkPropertiesMultisetBehavior) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();

  const uint64_t kLinkId = 1;

  Flatland parent(linker);
  Flatland child(linker);
  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(linker, &parent, &child, kLinkId, &content_link, &graph_link);
  RunLoopUntilIdle();

  const float kFinalSize = 100.0f;

  // Set the logical size to something new multiple times.
  for (int i = 10; i >= 0; --i) {
    LinkProperties properties;
    properties.set_logical_size({kFinalSize + i + 1.0f, kFinalSize + i + 1.0f});
    parent.SetLinkProperties(kLinkId, std::move(properties));
    LinkProperties properties2;
    properties2.set_logical_size({kFinalSize + i, kFinalSize + i});
    parent.SetLinkProperties(kLinkId, std::move(properties2));
    PRESENT(parent, true);
  }

  // Confirm that the callback is fired once, and that it has the most up-to-date data.
  {
    int num_updates = 0;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kFinalSize, info.logical_size().x);
      EXPECT_EQ(kFinalSize, info.logical_size().y);
      ++num_updates;
    });

    EXPECT_EQ(0, num_updates);
    RunLoopUntilIdle();
    EXPECT_EQ(1, num_updates);
  }

  const float kNewSize = 50.0f;

  // Confirm that calling GetLayout again results in a hung get.
  int num_updates = 0;
  graph_link->GetLayout([&](LayoutInfo info) {
    // When we receive the new layout information, confirm that we receive the first update in the
    // batch.
    //
    // TODO(36467): We should not be receiving updates involving data that is only accurate halfway
    // through the "atomic" application of a batch of operations.
    EXPECT_EQ(kNewSize, info.logical_size().x);
    EXPECT_EQ(kNewSize, info.logical_size().y);
    ++num_updates;
  });

  EXPECT_EQ(0, num_updates);
  RunLoopUntilIdle();
  EXPECT_EQ(0, num_updates);

  // Update the properties twice, once with a new value, with with the old value.
  {
    LinkProperties properties;
    properties.set_logical_size({kNewSize, kNewSize});
    parent.SetLinkProperties(kLinkId, std::move(properties));
    LinkProperties properties2;
    properties2.set_logical_size({kFinalSize, kFinalSize});
    parent.SetLinkProperties(kLinkId, std::move(properties2));
    PRESENT(parent, true);
  }

  // Confirm that we receive the update.
  EXPECT_EQ(0, num_updates);
  RunLoopUntilIdle();
  EXPECT_EQ(1, num_updates);
}

TEST_F(FlatlandTestLoop, SetLinkPropertiesOnMultipleChildren) {
  auto linker = std::make_shared<Flatland::ObjectLinker>();

  const int kNumChildren = 3;
  const uint64_t kLinkIds[3] = {1, 2, 3};

  Flatland parent(linker);
  Flatland children[kNumChildren] = {Flatland(linker), Flatland(linker), Flatland(linker)};
  fidl::InterfacePtr<ContentLink> content_link[kNumChildren];
  fidl::InterfacePtr<GraphLink> graph_link[kNumChildren];

  for (int i = 0; i < kNumChildren; ++i) {
    CreateLink(linker, &parent, &children[i], kLinkIds[i], &content_link[i], &graph_link[i]);
  }
  RunLoopUntilIdle();

  const float kDefaultSize = 1.0f;

  // Confirm that all children are at the default value
  for (int i = 0; i < kNumChildren; ++i) {
    bool layout_updated = false;
    graph_link[i]->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kDefaultSize, info.logical_size().x);
      EXPECT_EQ(kDefaultSize, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    RunLoopUntilIdle();
    EXPECT_TRUE(layout_updated);
  }

  // Resize the content on all children.
  for (auto id : kLinkIds) {
    LinkProperties properties;
    properties.set_logical_size({static_cast<float>(id), id * 2.0f});
    parent.SetLinkProperties(id, std::move(properties));
  }

  PRESENT(parent, true);

  for (int i = 0; i < kNumChildren; ++i) {
    bool layout_updated = false;
    graph_link[i]->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kLinkIds[i], info.logical_size().x);
      EXPECT_EQ(kLinkIds[i] * 2.0f, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    RunLoopUntilIdle();
    EXPECT_TRUE(layout_updated);
  }
}

#undef PRESENT

}  // namespace test
}  // namespace scenic_impl
