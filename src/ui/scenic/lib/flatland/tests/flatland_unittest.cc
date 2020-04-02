// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/flatland/flatland.h"

#include <limits>

#include <gtest/gtest.h>

#include "fuchsia/ui/scenic/internal/cpp/fidl.h"
#include "lib/gtest/test_loop_fixture.h"
#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/fxl/logging.h"
#include "src/ui/scenic/lib/flatland/global_matrix_data.h"
#include "src/ui/scenic/lib/flatland/global_topology_data.h"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtx/matrix_transform_2d.hpp>

using flatland::Flatland;
using LinkId = flatland::Flatland::LinkId;
using flatland::GlobalMatrixVector;
using flatland::GlobalTopologyData;
using flatland::LinkSystem;
using flatland::TransformGraph;
using flatland::TransformHandle;
using flatland::UberStructSystem;
using fuchsia::ui::scenic::internal::ContentLink;
using fuchsia::ui::scenic::internal::ContentLinkStatus;
using fuchsia::ui::scenic::internal::ContentLinkToken;
using fuchsia::ui::scenic::internal::Flatland_Present_Result;
using fuchsia::ui::scenic::internal::GraphLink;
using fuchsia::ui::scenic::internal::GraphLinkToken;
using fuchsia::ui::scenic::internal::LayoutInfo;
using fuchsia::ui::scenic::internal::LinkProperties;
using fuchsia::ui::scenic::internal::Orientation;
using fuchsia::ui::scenic::internal::Vec2;

// These macros are so that, if the various test macros fail, we get a line number associated with a
// particular Present() call in a unit test.
//
// |flatland| is a Flatland object. |expect_success| should be false if the call to Present() is
// expected to trigger an error.
#define PRESENT(flatland, expect_success)                                             \
  {                                                                                   \
    bool processed_callback = false;                                                  \
    flatland.Present([&](Flatland_Present_Result result) {                            \
      EXPECT_EQ(!expect_success, result.is_err());                                    \
      if (expect_success) {                                                           \
        EXPECT_EQ(1u, result.response().num_presents_remaining);                      \
      } else {                                                                        \
        EXPECT_EQ(fuchsia::ui::scenic::internal::Error::BAD_OPERATION, result.err()); \
      }                                                                               \
      processed_callback = true;                                                      \
    });                                                                               \
    EXPECT_TRUE(processed_callback);                                                  \
  }

// |global_topology_data| is a GlobalTopologyData object. |global_matrix_vector| is a
// GlobalMatrixVector generated from the same set of UberStructs and topology data. |target_handle|
// is the TransformHandle of the matrix to compare. |expected_matrix| is the expected value of
// that matrix.
#define EXPECT_MATRIX(global_topology_data, global_matrix_vector, target_handle, expected_matrix) \
  {                                                                                               \
    ASSERT_EQ(global_topology_data.live_handles.count(target_handle), 1u);                        \
    int index = -1;                                                                               \
    for (size_t i = 0; i < global_topology_data.topology_vector.size(); ++i) {                    \
      if (global_topology_data.topology_vector[i] == target_handle) {                             \
        index = i;                                                                                \
        break;                                                                                    \
      }                                                                                           \
    }                                                                                             \
    ASSERT_NE(index, -1);                                                                         \
    const glm::mat3& matrix = global_matrix_vector[index];                                        \
    for (size_t i = 0; i < 3; ++i) {                                                              \
      for (size_t j = 0; j < 3; ++j) {                                                            \
        EXPECT_FLOAT_EQ(matrix[i][j], expected_matrix[i][j]) << " row " << j << " column " << i;  \
      }                                                                                           \
    }                                                                                             \
  }

namespace {

const float kDefaultSize = 1.0f;

void CreateLink(Flatland* parent, Flatland* child, LinkId id,
                fidl::InterfacePtr<ContentLink>* content_link,
                fidl::InterfacePtr<GraphLink>* graph_link) {
  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  parent->CreateLink(id, std::move(parent_token), std::move(properties),
                     content_link->NewRequest());
  child->LinkToParent(std::move(child_token), graph_link->NewRequest());
  PRESENT((*parent), true);
  PRESENT((*child), true);
}

float GetOrientationAngle(fuchsia::ui::scenic::internal::Orientation orientation) {
  switch (orientation) {
    case Orientation::CCW_0_DEGREES:
      return 0.f;
    case Orientation::CCW_90_DEGREES:
      return glm::half_pi<float>();
    case Orientation::CCW_180_DEGREES:
      return glm::pi<float>();
    case Orientation::CCW_270_DEGREES:
      return glm::three_over_two_pi<float>();
  }
}

class FlatlandTest : public gtest::TestLoopFixture {
 public:
  FlatlandTest()
      : uber_struct_system_(std::make_shared<UberStructSystem>()),
        link_system_(std::make_shared<LinkSystem>(uber_struct_system_->GetNextInstanceId())) {}

  void TearDown() override { EXPECT_EQ(uber_struct_system_->GetSize(), 0u); }

  Flatland CreateFlatland() { return Flatland(link_system_, uber_struct_system_); }

  // The parent transform must be a topology root or ComputeGlobalTopologyData() will crash.
  bool IsDescendantOf(TransformHandle parent, TransformHandle child) {
    auto snapshot = uber_struct_system_->Snapshot();
    auto links = link_system_->GetResolvedTopologyLinks();
    auto data = GlobalTopologyData::ComputeGlobalTopologyData(
        snapshot, links, link_system_->GetInstanceId(), parent);
    for (auto handle : data.topology_vector) {
      if (handle == child) {
        return true;
      }
    }
    return false;
  }

  struct GlobalFlatlandData {
    GlobalTopologyData topology_data;
    GlobalMatrixVector matrix_vector;
  };

  // Processing the main loop involves generating a global topology. For testing, the root transform
  // is provided directly to this function.
  GlobalFlatlandData ProcessMainLoop(TransformHandle root_transform) {
    // Run the looper in case there are queued commands in, e.g., ObjectLinker.
    RunLoopUntilIdle();

    // This is a replica of the core render loop.
    auto snapshot = uber_struct_system_->Snapshot();
    auto links = link_system_->GetResolvedTopologyLinks();
    auto data = GlobalTopologyData::ComputeGlobalTopologyData(
        snapshot, links, link_system_->GetInstanceId(), root_transform);
    auto matrices = ComputeGlobalMatrixData(data.topology_vector, data.parent_indices, snapshot);

    link_system_->UpdateLinks(data.topology_vector, data.child_counts, data.live_handles, snapshot);

    // Run the looper again to process any queued FIDL events (i.e., Link callbacks).
    RunLoopUntilIdle();

    return {.topology_data = std::move(data), .matrix_vector = std::move(matrices)};
  }

  const std::shared_ptr<UberStructSystem> uber_struct_system_;
  const std::shared_ptr<LinkSystem> link_system_;
};

}  // namespace

namespace flatland {
namespace test {

TEST_F(FlatlandTest, PresentShouldReturnOne) {
  Flatland flatland = CreateFlatland();
  PRESENT(flatland, true);
}

TEST_F(FlatlandTest, CreateAndReleaseTransformValidCases) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, CreateAndReleaseTransformErrorCases) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, AddAndRemoveChildValidCases) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, AddAndRemoveChildErrorCases) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, MultichildUsecase) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, CycleDetector) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, SetRootTransform) {
  Flatland flatland = CreateFlatland();

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

TEST_F(FlatlandTest, SetTranslationErrorCases) {
  Flatland flatland = CreateFlatland();

  const uint64_t kIdNotCreated = 1;

  // Zero is not a valid transform ID.
  flatland.SetTranslation(0, {1.f, 2.f});
  PRESENT(flatland, false);

  // Transform does not exist.
  flatland.SetTranslation(kIdNotCreated, {1.f, 2.f});
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, SetOrientationErrorCases) {
  Flatland flatland = CreateFlatland();

  const uint64_t kIdNotCreated = 1;

  // Zero is not a valid transform ID.
  flatland.SetOrientation(0, Orientation::CCW_90_DEGREES);
  PRESENT(flatland, false);

  // Transform does not exist.
  flatland.SetOrientation(kIdNotCreated, Orientation::CCW_90_DEGREES);
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, SetScaleErrorCases) {
  Flatland flatland = CreateFlatland();

  const uint64_t kIdNotCreated = 1;

  // Zero is not a valid transform ID.
  flatland.SetScale(0, {1.f, 2.f});
  PRESENT(flatland, false);

  // Transform does not exist.
  flatland.SetScale(kIdNotCreated, {1.f, 2.f});
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, SetGeometricTransformProperties) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId1, &content_link, &graph_link);
  RunLoopUntilIdle();

  // Create two Transforms in the parent to ensure properties cascade down children.
  const uint64_t kId1 = 1;
  const uint64_t kId2 = 2;

  parent.CreateTransform(kId1);
  parent.CreateTransform(kId2);

  parent.SetRootTransform(kId1);
  parent.AddChild(kId1, kId2);

  parent.SetLinkOnTransform(kLinkId1, kId2);

  PRESENT(parent, true);

  // With no properties set, the child root has an identity transform.
  auto data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), glm::mat3());

  // Set up one property per transform. Set up kId2 before kId1 to ensure the hierarchy is used.
  parent.SetScale(kId2, {2.f, 3.f});
  parent.SetTranslation(kId1, {1.f, 2.f});
  PRESENT(parent, true);

  // The operations should be applied in the following order:
  // - Translation on kId1
  // - Scale on kId2
  glm::mat3 expected_matrix = glm::mat3();
  expected_matrix = glm::translate(expected_matrix, {1.f, 2.f});
  expected_matrix = glm::scale(expected_matrix, {2.f, 3.f});

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_matrix);

  // Fill out the remaining properties on both transforms.
  parent.SetOrientation(kId1, Orientation::CCW_90_DEGREES);
  parent.SetScale(kId1, {4.f, 5.f});
  parent.SetTranslation(kId2, {6.f, 7.f});
  parent.SetOrientation(kId2, Orientation::CCW_270_DEGREES);
  PRESENT(parent, true);

  // The operations should be applied in the following order:
  // - Translation on kId1
  // - Orientation on kId1
  // - Scale on kId1
  // - Translation on kId2
  // - Orientation on kId2
  // - Scale on kId2
  expected_matrix = glm::mat3();
  expected_matrix = glm::translate(expected_matrix, {1.f, 2.f});
  expected_matrix = glm::rotate(expected_matrix, GetOrientationAngle(Orientation::CCW_90_DEGREES));
  expected_matrix = glm::scale(expected_matrix, {4.f, 5.f});
  expected_matrix = glm::translate(expected_matrix, {6.f, 7.f});
  expected_matrix = glm::rotate(expected_matrix, GetOrientationAngle(Orientation::CCW_270_DEGREES));
  expected_matrix = glm::scale(expected_matrix, {2.f, 3.f});

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_matrix);

  // Ensure releasing one of the intermediate transforms does not clean up the matrix data since
  // it is still referenced in an active chain of Transforms.
  parent.ReleaseTransform(kId2);
  PRESENT(parent, true);

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_matrix);
}

TEST_F(FlatlandTest, GraphLinkReplaceWithoutConnection) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<GraphLink> graph_link;
  flatland.LinkToParent(std::move(child_token), graph_link.NewRequest());

  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, true);
  ProcessMainLoop(flatland.GetRoot());

  ContentLinkToken parent_token2;
  GraphLinkToken child_token2;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token2.value, &child_token2.value));

  fidl::InterfacePtr<GraphLink> graph_link2;
  flatland.LinkToParent(std::move(child_token2), graph_link2.NewRequest());

  // Until Present() is called, the previous GraphLink is not unbound.
  EXPECT_TRUE(graph_link.is_bound());
  EXPECT_TRUE(graph_link2.is_bound());

  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, true);
  ProcessMainLoop(flatland.GetRoot());

  EXPECT_FALSE(graph_link.is_bound());
  EXPECT_TRUE(graph_link2.is_bound());
}

TEST_F(FlatlandTest, GraphLinkReplaceWithConnection) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId1, &content_link, &graph_link);
  ProcessMainLoop(parent.GetRoot());

  fidl::InterfacePtr<GraphLink> graph_link2;

  // Don't use the helper function for the second link to test when the previous links are closed.
  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  // Creating the new GraphLink doesn't invalidate either of the old links until Present() is
  // called on the child.
  child.LinkToParent(std::move(child_token), graph_link2.NewRequest());

  EXPECT_TRUE(content_link.is_bound());
  EXPECT_TRUE(graph_link.is_bound());
  EXPECT_TRUE(graph_link2.is_bound());

  // Present() replaces the original GraphLink, which also results in the invalidation of both ends
  // of the original link.
  ProcessMainLoop(parent.GetRoot());
  PRESENT(child, true);
  ProcessMainLoop(parent.GetRoot());

  EXPECT_FALSE(content_link.is_bound());
  EXPECT_FALSE(graph_link.is_bound());
  EXPECT_TRUE(graph_link2.is_bound());
}

TEST_F(FlatlandTest, GraphLinkUnbindsOnParentDeath) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<GraphLink> graph_link;
  flatland.LinkToParent(std::move(child_token), graph_link.NewRequest());

  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, true);
  ProcessMainLoop(flatland.GetRoot());

  parent_token.value.reset();
  ProcessMainLoop(flatland.GetRoot());

  EXPECT_FALSE(graph_link.is_bound());
}

TEST_F(FlatlandTest, GraphLinkUnbindsImmediatelyWithInvalidToken) {
  Flatland flatland = CreateFlatland();

  GraphLinkToken child_token;

  fidl::InterfacePtr<GraphLink> graph_link;
  flatland.LinkToParent(std::move(child_token), graph_link.NewRequest());

  // The link will be unbound even before Present() is called.
  RunLoopUntilIdle();
  EXPECT_FALSE(graph_link.is_bound());

  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, GraphUnlinkFailsWithoutLink) {
  Flatland flatland = CreateFlatland();

  flatland.UnlinkFromParent([](GraphLinkToken token) { EXPECT_TRUE(false); });

  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, GraphUnlinkReturnsOrphanedTokenOnParentDeath) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<GraphLink> graph_link;
  flatland.LinkToParent(std::move(child_token), graph_link.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  // Killing the peer token does not prevent the instance from returning a valid token.
  parent_token.value.reset();
  RunLoopUntilIdle();

  GraphLinkToken graph_token;
  flatland.UnlinkFromParent(
      [&graph_token](GraphLinkToken token) { graph_token = std::move(token); });

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  EXPECT_TRUE(graph_token.value.is_valid());

  // But trying to link with that token will immediately fail because it is already orphaned.
  fidl::InterfacePtr<GraphLink> graph_link2;
  flatland.LinkToParent(std::move(graph_token), graph_link2.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  EXPECT_FALSE(graph_link2.is_bound());
}

TEST_F(FlatlandTest, GraphUnlinkReturnsOriginalToken) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const zx_koid_t expected_koid = fsl::GetKoid(child_token.value.get());

  fidl::InterfacePtr<GraphLink> graph_link;
  flatland.LinkToParent(std::move(child_token), graph_link.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  GraphLinkToken graph_token;
  flatland.UnlinkFromParent(
      [&graph_token](GraphLinkToken token) { graph_token = std::move(token); });

  // Until Present() is called, the previous GraphLink is not unbound.
  EXPECT_TRUE(graph_link.is_bound());
  EXPECT_FALSE(graph_token.value.is_valid());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  EXPECT_FALSE(graph_link.is_bound());
  EXPECT_TRUE(graph_token.value.is_valid());
  EXPECT_EQ(fsl::GetKoid(graph_token.value.get()), expected_koid);
}

TEST_F(FlatlandTest, ContentLinkUnbindsOnChildDeath) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  flatland.CreateLink(kLinkId1, std::move(parent_token), std::move(properties),
                      content_link.NewRequest());

  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, true);
  ProcessMainLoop(flatland.GetRoot());

  child_token.value.reset();
  ProcessMainLoop(flatland.GetRoot());

  EXPECT_FALSE(content_link.is_bound());
}

TEST_F(FlatlandTest, ContentLinkUnbindsImmediatelyWithInvalidToken) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  flatland.CreateLink(kLinkId1, std::move(parent_token), {}, content_link.NewRequest());

  // The link will be unbound even before Present() is called.
  RunLoopUntilIdle();
  EXPECT_FALSE(content_link.is_bound());

  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, ContentLinkIdIsZero) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  flatland.CreateLink(0, std::move(parent_token), std::move(properties), content_link.NewRequest());
  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, false);
  ProcessMainLoop(flatland.GetRoot());
}

TEST_F(FlatlandTest, ContentLinkNoLogicalSize) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  flatland.CreateLink(0, std::move(parent_token), std::move(properties), content_link.NewRequest());
  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, ContentLinkInvalidLogicalSize) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  fidl::InterfacePtr<ContentLink> content_link;

  // The X value must be positive.
  LinkProperties properties;
  properties.set_logical_size({0.f, kDefaultSize});
  flatland.CreateLink(0, std::move(parent_token), std::move(properties), content_link.NewRequest());
  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, false);

  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  // The Y value must be positive.
  LinkProperties properties2;
  properties2.set_logical_size({kDefaultSize, 0.f});
  flatland.CreateLink(0, std::move(parent_token), std::move(properties2),
                      content_link.NewRequest());
  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, ContentLinkIdCollision) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  flatland.CreateLink(kId1, std::move(parent_token), std::move(properties),
                      content_link.NewRequest());
  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, true);
  ProcessMainLoop(flatland.GetRoot());

  ContentLinkToken parent_token2;
  GraphLinkToken child_token2;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token2.value, &child_token2.value));

  ProcessMainLoop(flatland.GetRoot());
  flatland.CreateLink(kId1, std::move(parent_token2), std::move(properties),
                      content_link.NewRequest());
  ProcessMainLoop(flatland.GetRoot());
  PRESENT(flatland, false);
}

// This code doesn't use the helper function to create a link, because it tests intermediate steps
// and timing corner cases.
TEST_F(FlatlandTest, ValidParentToChildFlow) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kLinkId = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({1.0f, 2.0f});
  parent.CreateLink(kLinkId, std::move(parent_token), std::move(properties),
                    content_link.NewRequest());

  fidl::InterfacePtr<GraphLink> graph_link;
  child.LinkToParent(std::move(child_token), graph_link.NewRequest());

  bool layout_updated = false;
  graph_link->GetLayout([&](LayoutInfo info) {
    EXPECT_EQ(1.0f, info.logical_size().x);
    EXPECT_EQ(2.0f, info.logical_size().y);
    layout_updated = true;
  });

  // Without even presenting, the child is able to get the initial properties from the parent.
  ProcessMainLoop(parent.GetRoot());
  EXPECT_TRUE(layout_updated);
}

// This code doesn't use the helper function to create a link, because it tests intermediate steps
// and timing corner cases.
TEST_F(FlatlandTest, ValidChildToParentFlow) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kTransformId = 1;
  const uint64_t kLinkId = 2;

  parent.CreateTransform(kTransformId);
  parent.SetRootTransform(kTransformId);
  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({1.0f, 2.0f});
  parent.CreateLink(kLinkId, std::move(parent_token), std::move(properties),
                    content_link.NewRequest());
  parent.SetLinkOnTransform(kLinkId, kTransformId);

  fidl::InterfacePtr<GraphLink> graph_link;
  child.LinkToParent(std::move(child_token), graph_link.NewRequest());

  bool status_updated = false;
  content_link->GetStatus([&](ContentLinkStatus status) {
    ASSERT_EQ(ContentLinkStatus::CONTENT_HAS_PRESENTED, status);
    status_updated = true;
  });

  // The content link status cannot change until both parties have presented -- the parent Flatland
  // instance must Present() so that the graph is part of the global topology, and the child
  // Flatland instance must Present() so that CONTENT_HAS_PRESENTED can be true.
  EXPECT_FALSE(status_updated);
  PRESENT(parent, true);
  ProcessMainLoop(parent.GetRoot());
  PRESENT(child, true);
  EXPECT_FALSE(status_updated);
  ProcessMainLoop(parent.GetRoot());
  EXPECT_TRUE(status_updated);
}

TEST_F(FlatlandTest, SetLinkPropertiesDefaultBehavior) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kTransformId = 1;
  const uint64_t kLinkId = 2;

  parent.CreateTransform(kTransformId);
  parent.SetRootTransform(kTransformId);
  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId, &content_link, &graph_link);
  parent.SetLinkOnTransform(kLinkId, kTransformId);
  ProcessMainLoop(parent.GetRoot());

  // Confirm that the current layout is the default.
  {
    bool layout_updated = false;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kDefaultSize, info.logical_size().x);
      EXPECT_EQ(kDefaultSize, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    ProcessMainLoop(parent.GetRoot());
    EXPECT_TRUE(layout_updated);
  }

  // Set the logical size to something new.
  {
    LinkProperties properties;
    properties.set_logical_size({2.0f, 3.0f});
    parent.SetLinkProperties(kLinkId, std::move(properties));
    PRESENT(parent, true);
  }

  // Confirm that the new logical size is accessible.
  {
    bool layout_updated = false;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(2.0f, info.logical_size().x);
      EXPECT_EQ(3.0f, info.logical_size().y);
      layout_updated = true;
    });

    EXPECT_FALSE(layout_updated);
    ProcessMainLoop(parent.GetRoot());
    EXPECT_TRUE(layout_updated);
  }

  // Set link properties using a properties object with an unset size field.
  {
    LinkProperties default_properties;
    parent.SetLinkProperties(kLinkId, std::move(default_properties));
    PRESENT(parent, true);
  }

  // Confirm that no update has been triggered.
  {
    bool layout_updated = false;
    graph_link->GetLayout([&](LayoutInfo info) { layout_updated = true; });

    EXPECT_FALSE(layout_updated);
    ProcessMainLoop(parent.GetRoot());
    EXPECT_FALSE(layout_updated);
  }
}

TEST_F(FlatlandTest, SetLinkPropertiesMultisetBehavior) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kTransformId = 1;
  const uint64_t kLinkId = 2;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId, &content_link, &graph_link);

  // Our initial layout (from link creation) should be the default size.
  {
    int num_updates = 0;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kDefaultSize, info.logical_size().x);
      EXPECT_EQ(kDefaultSize, info.logical_size().y);
      ++num_updates;
    });

    EXPECT_EQ(0, num_updates);
    ProcessMainLoop(parent.GetRoot());
    EXPECT_EQ(1, num_updates);
  }

  // Create a full chain of transforms from parent root to child root.
  parent.CreateTransform(kTransformId);
  parent.SetRootTransform(kTransformId);
  parent.SetLinkOnTransform(kLinkId, kTransformId);
  PRESENT(parent, true);

  const float kInitialSize = 100.0f;

  // Set the logical size to something new multiple times.
  for (int i = 10; i >= 0; --i) {
    LinkProperties properties;
    properties.set_logical_size({kInitialSize + i + 1.0f, kInitialSize + i + 1.0f});
    parent.SetLinkProperties(kLinkId, std::move(properties));
    LinkProperties properties2;
    properties2.set_logical_size({kInitialSize + i, kInitialSize + i});
    parent.SetLinkProperties(kLinkId, std::move(properties2));
    PRESENT(parent, true);
  }

  // Confirm that the callback is fired once, and that it has the most up-to-date data.
  {
    int num_updates = 0;
    graph_link->GetLayout([&](LayoutInfo info) {
      EXPECT_EQ(kInitialSize, info.logical_size().x);
      EXPECT_EQ(kInitialSize, info.logical_size().y);
      ++num_updates;
    });

    EXPECT_EQ(0, num_updates);
    ProcessMainLoop(parent.GetRoot());
    EXPECT_EQ(1, num_updates);
  }

  const float kNewSize = 50.0f;

  // Confirm that calling GetLayout again results in a hung get.
  int num_updates = 0;
  graph_link->GetLayout([&](LayoutInfo info) {
    // When we receive the new layout information, confirm that we receive the last update in the
    // batch.
    EXPECT_EQ(kNewSize, info.logical_size().x);
    EXPECT_EQ(kNewSize, info.logical_size().y);
    ++num_updates;
  });

  EXPECT_EQ(0, num_updates);
  ProcessMainLoop(parent.GetRoot());
  EXPECT_EQ(0, num_updates);

  // Update the properties twice, once with the old value, once with the new value.
  {
    LinkProperties properties;
    properties.set_logical_size({kInitialSize, kInitialSize});
    parent.SetLinkProperties(kLinkId, std::move(properties));
    LinkProperties properties2;
    properties2.set_logical_size({kNewSize, kNewSize});
    parent.SetLinkProperties(kLinkId, std::move(properties2));
    PRESENT(parent, true);
  }

  // Confirm that we receive the update.
  EXPECT_EQ(0, num_updates);
  ProcessMainLoop(parent.GetRoot());
  EXPECT_EQ(1, num_updates);
}

TEST_F(FlatlandTest, SetLinkPropertiesOnMultipleChildren) {
  const int kNumChildren = 3;
  const uint64_t kRootTransform = 1;
  const uint64_t kTransformIds[kNumChildren] = {2, 3, 4};
  const uint64_t kLinkIds[kNumChildren] = {5, 6, 7};

  Flatland parent = CreateFlatland();
  Flatland children[kNumChildren] = {CreateFlatland(), CreateFlatland(), CreateFlatland()};
  fidl::InterfacePtr<ContentLink> content_link[kNumChildren];
  fidl::InterfacePtr<GraphLink> graph_link[kNumChildren];

  parent.CreateTransform(kRootTransform);
  parent.SetRootTransform(kRootTransform);

  for (int i = 0; i < kNumChildren; ++i) {
    parent.CreateTransform(kTransformIds[i]);
    parent.AddChild(kRootTransform, kTransformIds[i]);
    CreateLink(&parent, &children[i], kLinkIds[i], &content_link[i], &graph_link[i]);
    parent.SetLinkOnTransform(kLinkIds[i], kTransformIds[i]);
  }
  ProcessMainLoop(parent.GetRoot());

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
    ProcessMainLoop(parent.GetRoot());
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
    ProcessMainLoop(parent.GetRoot());
    EXPECT_TRUE(layout_updated);
  }
}

TEST_F(FlatlandTest, SetLinkOnTransformErrorCases) {
  Flatland flatland = CreateFlatland();

  // Setup.

  const uint64_t kId1 = 1;
  const uint64_t kId2 = 2;

  flatland.CreateTransform(kId1);

  const uint64_t kLinkId1 = 1;
  const uint64_t kLinkId2 = 2;

  fidl::InterfacePtr<ContentLink> content_link;

  // Creating a link with an empty property object is an error. Logical size must be provided at
  // creation time.
  {
    ContentLinkToken parent_token;
    GraphLinkToken child_token;
    ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));
    LinkProperties empty_properties;
    flatland.CreateLink(kLinkId1, std::move(parent_token), std::move(empty_properties),
                        content_link.NewRequest());

    PRESENT(flatland, false);
  }

  // We have to recreate our tokens to get a valid link object.
  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  flatland.CreateLink(kLinkId1, std::move(parent_token), std::move(properties),
                      content_link.NewRequest());

  PRESENT(flatland, true);

  // Zero is not a valid transform_id.
  flatland.SetLinkOnTransform(kLinkId1, 0);
  PRESENT(flatland, false);

  // Setting a valid link on an ivnalid transform is not valid.
  flatland.SetLinkOnTransform(kLinkId1, kId2);
  PRESENT(flatland, false);

  // Setting an invalid link on a valid transform is not valid.
  flatland.SetLinkOnTransform(kLinkId2, kId1);
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, ReleaseLinkErrorCases) {
  Flatland flatland = CreateFlatland();

  // Zero is not a valid link_id.
  flatland.ReleaseLink(0, [](ContentLinkToken token) { EXPECT_TRUE(false); });
  PRESENT(flatland, false);

  // Using a link_id that does not exist is not valid.
  const uint64_t kLinkId1 = 1;
  flatland.ReleaseLink(kLinkId1, [](ContentLinkToken token) { EXPECT_TRUE(false); });
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, ReleaseLinkReturnsOriginalToken) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const zx_koid_t expected_koid = fsl::GetKoid(parent_token.value.get());

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  flatland.CreateLink(kLinkId1, std::move(parent_token), std::move(properties),
                      content_link.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  ContentLinkToken content_token;
  flatland.ReleaseLink(
      kLinkId1, [&content_token](ContentLinkToken token) { content_token = std::move(token); });

  // Until Present() is called, the previous ContentLink is not unbound.
  EXPECT_TRUE(content_link.is_bound());
  EXPECT_FALSE(content_token.value.is_valid());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  EXPECT_FALSE(content_link.is_bound());
  EXPECT_TRUE(content_token.value.is_valid());
  EXPECT_EQ(fsl::GetKoid(content_token.value.get()), expected_koid);
}

TEST_F(FlatlandTest, ReleaseLinkReturnsOrphanedTokenOnChildDeath) {
  Flatland flatland = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  flatland.CreateLink(kLinkId1, std::move(parent_token), std::move(properties),
                      content_link.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  // Killing the peer token does not prevent the instance from returning a valid token.
  child_token.value.reset();
  RunLoopUntilIdle();

  ContentLinkToken content_token;
  flatland.ReleaseLink(
      kLinkId1, [&content_token](ContentLinkToken token) { content_token = std::move(token); });

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  EXPECT_TRUE(content_token.value.is_valid());

  // But trying to link with that token will immediately fail because it is already orphaned.
  const uint64_t kLinkId2 = 2;

  fidl::InterfacePtr<ContentLink> content_link2;
  flatland.CreateLink(kLinkId2, std::move(content_token), std::move(properties),
                      content_link2.NewRequest());

  RunLoopUntilIdle();
  PRESENT(flatland, true);
  RunLoopUntilIdle();

  EXPECT_FALSE(content_link2.is_bound());
}

TEST_F(FlatlandTest, CreateLinkPresentedBeforeLinkToParent) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  // Create a transform, add it to the parent, then create a link and assign to the transform.
  const uint64_t kId1 = 1;
  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);

  const uint64_t kLinkId = 1;

  fidl::InterfacePtr<ContentLink> parent_content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  parent.CreateLink(kLinkId, std::move(parent_token), std::move(properties),
                    parent_content_link.NewRequest());
  parent.SetLinkOnTransform(kLinkId, kId1);

  PRESENT(parent, true);

  // Link the child to the parent.
  fidl::InterfacePtr<GraphLink> child_graph_link;
  child.LinkToParent(std::move(child_token), child_graph_link.NewRequest());

  // The child should only be accessible from the parent when Present() is called on the child.
  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  PRESENT(child, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));
}

TEST_F(FlatlandTest, LinkToParentPresentedBeforeCreateLink) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  // Link the child to the parent
  fidl::InterfacePtr<GraphLink> child_graph_link;
  child.LinkToParent(std::move(child_token), child_graph_link.NewRequest());

  PRESENT(child, true);

  // Create a transform, add it to the parent, then create a link and assign to the transform.
  const uint64_t kId1 = 1;
  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);

  // Present the parent once so that it has a topology or else IsDescendantOf() will crash.
  PRESENT(parent, true);

  const uint64_t kLinkId = 1;

  fidl::InterfacePtr<ContentLink> parent_content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  parent.CreateLink(kLinkId, std::move(parent_token), std::move(properties),
                    parent_content_link.NewRequest());
  parent.SetLinkOnTransform(kLinkId, kId1);

  // The child should only be accessible from the parent when Present() is called on the parent.
  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  PRESENT(parent, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));
}

TEST_F(FlatlandTest, LinkResolvedBeforeEitherPresent) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  // Create a transform, add it to the parent, then create a link and assign to the transform.
  const uint64_t kId1 = 1;
  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);

  // Present the parent once so that it has a topology or else IsDescendantOf() will crash.
  PRESENT(parent, true);

  const uint64_t kLinkId = 1;

  fidl::InterfacePtr<ContentLink> parent_content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  parent.CreateLink(kLinkId, std::move(parent_token), std::move(properties),
                    parent_content_link.NewRequest());
  parent.SetLinkOnTransform(kLinkId, kId1);

  // Link the child to the parent.
  fidl::InterfacePtr<GraphLink> child_graph_link;
  child.LinkToParent(std::move(child_token), child_graph_link.NewRequest());

  // The child should only be accessible from the parent when Present() is called on both the parent
  // and the child.
  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  PRESENT(parent, true);

  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  PRESENT(child, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));
}

TEST_F(FlatlandTest, ClearChildLink) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  ContentLinkToken parent_token;
  GraphLinkToken child_token;
  ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &parent_token.value, &child_token.value));

  // Create and link the two instances.
  const uint64_t kId1 = 1;
  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);

  const uint64_t kLinkId = 1;

  fidl::InterfacePtr<ContentLink> parent_content_link;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  parent.CreateLink(kLinkId, std::move(parent_token), std::move(properties),
                    parent_content_link.NewRequest());
  parent.SetLinkOnTransform(kLinkId, kId1);

  fidl::InterfacePtr<GraphLink> child_graph_link;
  child.LinkToParent(std::move(child_token), child_graph_link.NewRequest());

  PRESENT(parent, true);
  PRESENT(child, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  // Reset the child link using zero as the link id.
  parent.SetLinkOnTransform(0, kId1);

  PRESENT(parent, true);

  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));
}

TEST_F(FlatlandTest, RelinkUnlinkedParentSameToken) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId1, &content_link, &graph_link);
  RunLoopUntilIdle();

  const uint64_t kId1 = 1;
  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);
  parent.SetLinkOnTransform(kId1, kLinkId1);

  PRESENT(parent, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  GraphLinkToken graph_token;
  child.UnlinkFromParent([&graph_token](GraphLinkToken token) { graph_token = std::move(token); });

  PRESENT(child, true);

  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  // The same token can be used to link a different instance.
  Flatland child2 = CreateFlatland();
  child2.LinkToParent(std::move(graph_token), graph_link.NewRequest());

  PRESENT(child2, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child2.GetRoot()));

  // The old instance is not re-linked.
  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));
}

TEST_F(FlatlandTest, RecreateReleasedLinkSameToken) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId1, &content_link, &graph_link);
  RunLoopUntilIdle();

  const uint64_t kId1 = 1;
  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);
  parent.SetLinkOnTransform(kId1, kLinkId1);

  PRESENT(parent, true);

  EXPECT_TRUE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  ContentLinkToken content_token;
  parent.ReleaseLink(
      kLinkId1, [&content_token](ContentLinkToken token) { content_token = std::move(token); });

  PRESENT(parent, true);

  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));

  // The same token can be used to create a different link to the same child with a different
  // parent.
  Flatland parent2 = CreateFlatland();

  const uint64_t kId2 = 2;
  parent2.CreateTransform(kId2);
  parent2.SetRootTransform(kId2);

  const uint64_t kLinkId2 = 2;
  LinkProperties properties;
  properties.set_logical_size({kDefaultSize, kDefaultSize});
  parent2.CreateLink(kLinkId2, std::move(content_token), std::move(properties),
                     content_link.NewRequest());
  parent2.SetLinkOnTransform(kId2, kLinkId2);

  PRESENT(parent2, true);

  EXPECT_TRUE(IsDescendantOf(parent2.GetRoot(), child.GetRoot()));

  // The old instance is not re-linked.
  EXPECT_FALSE(IsDescendantOf(parent.GetRoot(), child.GetRoot()));
}

TEST_F(FlatlandTest, SetLinkSizeErrorCases) {
  Flatland flatland = CreateFlatland();

  const uint64_t kIdNotCreated = 1;

  // Zero is not a valid transform ID.
  flatland.SetLinkSize(0, {1.f, 2.f});
  PRESENT(flatland, false);

  // Size contains non-positive components.
  flatland.SetLinkSize(0, {-1.f, 2.f});
  PRESENT(flatland, false);

  flatland.SetLinkSize(0, {1.f, 0.f});
  PRESENT(flatland, false);

  // Link does not exist.
  flatland.SetLinkSize(kIdNotCreated, {1.f, 2.f});
  PRESENT(flatland, false);
}

TEST_F(FlatlandTest, LinkSizeRatiosCreateScaleMatrix) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId1, &content_link, &graph_link);
  RunLoopUntilIdle();

  const uint64_t kId1 = 1;

  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);
  parent.SetLinkOnTransform(kLinkId1, kId1);

  PRESENT(parent, true);

  // The default size is the same as the logical size, so the child root has an identity matrix.
  // With no properties set, the child root has an identity transform.
  auto data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), glm::mat3());

  // Change the link size to half the width and a quarter the height.
  const float kNewLinkWidth = 0.5f * kDefaultSize;
  const float kNewLinkHeight = 0.25f * kDefaultSize;
  parent.SetLinkSize(kLinkId1, {kNewLinkWidth, kNewLinkHeight});

  PRESENT(parent, true);

  // This should change the expected matrix to apply the same scales.
  const glm::mat3 expected_scale_matrix = glm::scale(glm::mat3(), {kNewLinkWidth, kNewLinkHeight});

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_scale_matrix);

  // Changing the logical size to the same values returns the matrix to the identity matrix.
  LinkProperties properties;
  properties.set_logical_size({kNewLinkWidth, kNewLinkHeight});
  parent.SetLinkProperties(kLinkId1, std::move(properties));

  PRESENT(parent, true);

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), glm::mat3());

  // Change the logical size back to the default size.
  LinkProperties properties2;
  properties2.set_logical_size({kDefaultSize, kDefaultSize});
  parent.SetLinkProperties(kLinkId1, std::move(properties2));

  PRESENT(parent, true);

  // This should change the expected matrix back to applying the scales.
  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_scale_matrix);
}

TEST_F(FlatlandTest, EmptyLogicalSizePreservesOldSize) {
  Flatland parent = CreateFlatland();
  Flatland child = CreateFlatland();

  const uint64_t kLinkId1 = 1;

  fidl::InterfacePtr<ContentLink> content_link;
  fidl::InterfacePtr<GraphLink> graph_link;
  CreateLink(&parent, &child, kLinkId1, &content_link, &graph_link);
  RunLoopUntilIdle();

  const uint64_t kId1 = 1;

  parent.CreateTransform(kId1);
  parent.SetRootTransform(kId1);
  parent.SetLinkOnTransform(kLinkId1, kId1);

  PRESENT(parent, true);

  // Set the link size and logical size to new values
  const float kNewLinkWidth = 2.f * kDefaultSize;
  const float kNewLinkHeight = 3.f * kDefaultSize;
  parent.SetLinkSize(kLinkId1, {kNewLinkWidth, kNewLinkHeight});

  const float kNewLinkLogicalWidth = 5.f * kDefaultSize;
  const float kNewLinkLogicalHeight = 7.f * kDefaultSize;
  LinkProperties properties;
  properties.set_logical_size({kNewLinkLogicalWidth, kNewLinkLogicalHeight});
  parent.SetLinkProperties(kLinkId1, std::move(properties));

  PRESENT(parent, true);

  // This should result in an expected matrix that applies the ratio of the scales.
  glm::mat3 expected_scale_matrix = glm::scale(
      glm::mat3(), {kNewLinkWidth / kNewLinkLogicalWidth, kNewLinkHeight / kNewLinkLogicalHeight});

  auto data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_scale_matrix);

  // Setting a new LinkProperties with no logical size shouldn't change the matrix.
  LinkProperties properties2;
  parent.SetLinkProperties(kLinkId1, std::move(properties2));

  PRESENT(parent, true);

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_scale_matrix);

  // But it should still preserve the old logical size so that a subsequent link size update uses
  // the old logical size.
  const float kNewLinkWidth2 = 11.f * kDefaultSize;
  const float kNewLinkHeight2 = 13.f * kDefaultSize;
  parent.SetLinkSize(kLinkId1, {kNewLinkWidth2, kNewLinkHeight2});

  PRESENT(parent, true);

  // This should result in an expected matrix that applies the ratio of the scales.
  expected_scale_matrix = glm::scale(glm::mat3(), {kNewLinkWidth2 / kNewLinkLogicalWidth,
                                                   kNewLinkHeight2 / kNewLinkLogicalHeight});

  data = ProcessMainLoop(parent.GetRoot());
  EXPECT_MATRIX(data.topology_data, data.matrix_vector, child.GetRoot(), expected_scale_matrix);
}

#undef EXPECT_MATRIX
#undef PRESENT

}  // namespace test
}  // namespace flatland
