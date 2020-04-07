// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/fd.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/zx/event.h>

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "gmock/gmock.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/ui/a11y/bin/a11y_manager/tests/util/util.h"
#include "src/ui/a11y/lib/semantics/semantic_tree.h"
#include "src/ui/a11y/lib/semantics/tests/semantic_tree_parser.h"
#include "src/ui/a11y/lib/util/util.h"

namespace accessibility_test {
namespace {

using ::a11y::SemanticTree;
using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::Role;

// Valid tree paths.
const std::string kSemanticTreeSingleNodePath = "/pkg/data/semantic_tree_single_node.json";
const std::string kSemanticTreeOddNodesPath = "/pkg/data/semantic_tree_odd_nodes.json";
// Invalid tree paths.
const std::string kSemanticTreeWithCyclePath = "/pkg/data/cyclic_semantic_tree.json";
const std::string kSemanticTreeWithMissingChildrenPath =
    "/pkg/data/semantic_tree_not_parseable.json";

class SemanticTreeTest : public gtest::TestLoopFixture {
 public:
  SemanticTreeTest() {
    tree_.set_action_handler([this](uint32_t node_id,
                                    fuchsia::accessibility::semantics::Action action,
                                    fuchsia::accessibility::semantics::SemanticListener::
                                        OnAccessibilityActionRequestedCallback callback) {
      this->action_handler_called_ = true;
    });
    tree_.set_hit_testing_handler(
        [this](fuchsia::math::PointF local_point,
               fuchsia::accessibility::semantics::SemanticListener::HitTestCallback callback) {
          this->hit_testing_called_ = true;
        });
  }

 protected:
  void SetUp() override { TestLoopFixture::SetUp(); }

  // Checks if the tree contains all nodes  in |ids|.
  void TreeContainsNodes(const std::vector<uint32_t>& ids) {
    for (const auto id : ids) {
      auto node = tree_.GetNode(id);
      EXPECT_TRUE(node);
      EXPECT_EQ(node->node_id(), id);
    }
  }

  SemanticTree::TreeUpdates BuildUpdatesFromFile(const std::string& file_path) {
    SemanticTree::TreeUpdates updates;
    std::vector<Node> nodes;
    EXPECT_TRUE(semantic_tree_parser_.ParseSemanticTree(file_path, &nodes));
    for (auto& node : nodes) {
      updates.emplace_back(std::move(node));
    }
    return updates;
  }

  SemanticTreeParser semantic_tree_parser_;

  // Whether the action handler was called.
  bool action_handler_called_ = false;

  // Whether the hit testing handler was called.
  bool hit_testing_called_ = false;

  // Our test subject.
  SemanticTree tree_;
};

TEST_F(SemanticTreeTest, GetNodesById) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeSingleNodePath);

  EXPECT_TRUE(tree_.Update(std::move(updates)));

  // Attempt to retrieve node with id not present in tree.
  auto invalid_node = tree_.GetNode(1u);
  auto root = tree_.GetNode(SemanticTree::kRootNodeId);

  EXPECT_EQ(invalid_node, nullptr);
  EXPECT_EQ(root->node_id(), SemanticTree::kRootNodeId);
}

TEST_F(SemanticTreeTest, ClearsTheTree) {
  SemanticTree::TreeUpdates updates;
  updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
  updates.emplace_back(CreateTestNode(1u, "node1"));
  updates.emplace_back(CreateTestNode(2u, "node2"));

  EXPECT_TRUE(tree_.Update(std::move(updates)));
  EXPECT_EQ(tree_.Size(), 3u);
  tree_.Clear();
  EXPECT_EQ(tree_.Size(), 0u);
}

TEST_F(SemanticTreeTest, ReceivesTreeInOneSingleUpdate) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_.Update(std::move(updates)));
  TreeContainsNodes(added_ids);
}

TEST_F(SemanticTreeTest, BuildsTreeFromTheLeaves) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  // Updates is in ascending order. Sort it in descending order to send the
  // updates from the leaves.
  std::sort(updates.begin(), updates.end(),
            [](const auto& a, const auto& b) { return a.node().node_id() > b.node().node_id(); });

  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_.Update(std::move(updates)));
  TreeContainsNodes(added_ids);
}

TEST_F(SemanticTreeTest, InvalidTreeWithoutParent) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  // Remove the root (first node).
  updates.erase(updates.begin());
  EXPECT_FALSE(tree_.Update(std::move(updates)));
}

TEST_F(SemanticTreeTest, InvalidTreeWithCycle) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeWithCyclePath);
  EXPECT_FALSE(tree_.Update(std::move(updates)));
  EXPECT_EQ(tree_.Size(), 0u);
}

TEST_F(SemanticTreeTest, DeletingNodesByUpdatingTheParent) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_.Update(std::move(updates)));
  {
    auto root = tree_.GetNode(SemanticTree::kRootNodeId);
    EXPECT_EQ(root->attributes().label(), "Node-0");
    EXPECT_EQ(root->child_ids().size(), 2u);
  }
  // Update the root to point to nobody else.
  auto new_root = CreateTestNode(SemanticTree::kRootNodeId, "node1");
  new_root.set_child_ids(std::vector<uint32_t>());  // Points to no children.
  new_root.mutable_attributes()->set_label("new node");
  EXPECT_TRUE(new_root.has_child_ids());
  SemanticTree::TreeUpdates new_updates;
  new_updates.emplace_back(std::move(new_root));
  EXPECT_TRUE(tree_.Update(std::move(new_updates)));
  {
    auto root = tree_.GetNode(0);
    EXPECT_TRUE(root->child_ids().empty());
    EXPECT_EQ(root->attributes().label(), "new node");
  }
  EXPECT_EQ(tree_.Size(), 1u);
  for (const auto id : added_ids) {
    auto node = tree_.GetNode(id);
    if (id == SemanticTree::kRootNodeId) {
      EXPECT_TRUE(node);
      EXPECT_EQ(node->node_id(), id);
    } else {
      EXPECT_FALSE(node);
    }
  }
}

TEST_F(SemanticTreeTest, ExplicitlyDeletingNodes) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  std::vector<uint32_t> added_ids;
  for (const auto& update : updates) {
    added_ids.push_back(update.node().node_id());
  }
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  SemanticTree::TreeUpdates delete_updates;
  delete_updates.emplace_back(5);
  delete_updates.emplace_back(6);
  // Update the parent.
  auto updated_parent = CreateTestNode(2, "updated parent");
  *updated_parent.mutable_child_ids() = std::vector<uint32_t>();
  delete_updates.push_back(std::move(updated_parent));
  // Remove 5 and 6 from |added_ids|.
  auto it_5 = std::find(added_ids.begin(), added_ids.end(), 5);
  EXPECT_NE(it_5, added_ids.end());
  added_ids.erase(it_5);
  auto it_6 = std::find(added_ids.begin(), added_ids.end(), 6);
  EXPECT_NE(it_6, added_ids.end());
  added_ids.erase(it_6);
  EXPECT_TRUE(tree_.Update(std::move(delete_updates)));

  EXPECT_EQ(tree_.Size(), 5u);
  TreeContainsNodes(added_ids);
}

TEST_F(SemanticTreeTest, DeletingRootNodeClearsTheTree) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  SemanticTree::TreeUpdates delete_updates;
  delete_updates.emplace_back(SemanticTree::kRootNodeId);
  EXPECT_TRUE(tree_.Update(std::move(delete_updates)));

  EXPECT_EQ(tree_.Size(), 0u);
}

TEST_F(SemanticTreeTest, ReplaceNodeWithADeletion) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  SemanticTree::TreeUpdates delete_updates;
  delete_updates.emplace_back(2);
  delete_updates.emplace_back(CreateTestNode(2, "new node 2", {5, 6}));

  EXPECT_TRUE(tree_.Update(std::move(delete_updates)));

  EXPECT_EQ(tree_.Size(), 7u);
  auto node = tree_.GetNode(2);
  EXPECT_TRUE(node);
  EXPECT_THAT(node->attributes().label(), "new node 2");
  EXPECT_THAT(node->child_ids(), testing::ElementsAre(5, 6));
}

TEST_F(SemanticTreeTest, SemanticTreeWithMissingChildren) {
  SemanticTree::TreeUpdates updates;
  updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
  updates.emplace_back(CreateTestNode(1u, "node1"));
  updates.emplace_back(CreateTestNode(2u, "node2", {3}));
  EXPECT_FALSE(tree_.Update(std::move(updates)));
  EXPECT_EQ(tree_.Size(), 0u);
}

TEST_F(SemanticTreeTest, PartialUpdateCopiesNewInfo) {
  {
    SemanticTree::TreeUpdates updates;
    updates.emplace_back(CreateTestNode(SemanticTree::kRootNodeId, "node0", {1, 2}));
    updates.emplace_back(CreateTestNode(1u, "node1"));
    updates.emplace_back(CreateTestNode(2u, "node2"));
    EXPECT_TRUE(tree_.Update(std::move(updates)));
  }
  EXPECT_EQ(tree_.Size(), 3u);
  SemanticTree::TreeUpdates updates;
  // Partial update of the root node with a new label.
  // Please note that there are two partial updates on the root node, and the
  // partial update must always be applied on top of the existing one.
  // Sets additional fields to the node.
  auto first_root_update = CreateTestNode(SemanticTree::kRootNodeId, "root", {1, 2, 10});
  first_root_update.set_role(fuchsia::accessibility::semantics::Role::UNKNOWN);
  first_root_update.mutable_states()->set_selected(true);
  updates.emplace_back(std::move(first_root_update));
  auto second_root_update = CreateTestNode(SemanticTree::kRootNodeId, "updated label");
  second_root_update.mutable_states()->set_selected(false);
  updates.emplace_back(std::move(second_root_update));
  updates.emplace_back(CreateTestNode(10, "node 10"));

  EXPECT_TRUE(tree_.Update(std::move(updates)));
  EXPECT_EQ(tree_.Size(), 4u);
  auto root = tree_.GetNode(SemanticTree::kRootNodeId);
  EXPECT_EQ(root->attributes().label(), "updated label");

  // Check that prior data is still present.
  EXPECT_THAT(root->child_ids(), testing::ElementsAre(1, 2, 10));
  EXPECT_EQ(root->role(), fuchsia::accessibility::semantics::Role::UNKNOWN);
  EXPECT_FALSE(root->states().selected());
}

TEST_F(SemanticTreeTest, ReparentsNodes) {
  // A common use case of semantic trees is to reparent a node. Within an
  // update, reparenting would look like as a removal of a child node ID of one
  // node and the addition of that same child node ID to another node (new
  // parent).
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));
  SemanticTree::TreeUpdates reparenting_updates;
  reparenting_updates.push_back(
      CreateTestNode(SemanticTree::kRootNodeId, "root", {1}));  // 2 removed.
  reparenting_updates.push_back(
      CreateTestNode(1, "new parent", {3, 4, 2}));  // 2 will have 1 as new parent.
  EXPECT_TRUE(tree_.Update(std::move(reparenting_updates)));
  EXPECT_EQ(tree_.Size(), 7u);
  auto root = tree_.GetNode(SemanticTree::kRootNodeId);
  EXPECT_TRUE(root);
  EXPECT_THAT(root->child_ids(), testing::ElementsAre(1));
  auto new_parent = tree_.GetNode(1);
  EXPECT_TRUE(new_parent);
  EXPECT_THAT(new_parent->child_ids(), testing::ElementsAre(3, 4, 2));
}

TEST_F(SemanticTreeTest, GetParentNodeTest) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));
  auto parent = tree_.GetParentNode(1);
  auto missing_parent = tree_.GetParentNode(SemanticTree::kRootNodeId);
  EXPECT_TRUE(parent);
  EXPECT_FALSE(missing_parent);

  EXPECT_EQ(parent->node_id(), SemanticTree::kRootNodeId);
}

TEST_F(SemanticTreeTest, PerformAccessibilityActionRequested) {
  tree_.PerformAccessibilityAction(1, fuchsia::accessibility::semantics::Action::DEFAULT,
                                   [](auto...) {});
  EXPECT_TRUE(action_handler_called_);
}

TEST_F(SemanticTreeTest, PerformHitTestingRequested) {
  tree_.PerformHitTesting({1, 1}, [](auto...) {});
  EXPECT_TRUE(hit_testing_called_);
}

TEST_F(SemanticTreeTest, NextNodeExists) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetNextNode(1u);
  EXPECT_NE(next_node, nullptr);
  EXPECT_EQ(next_node->node_id(), 3u);
}

TEST_F(SemanticTreeTest, NoNextNode) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetNextNode(6u);
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, GetNextNodeForNonexistentId) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetNextNode(10u);
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, PreviousNodeExists) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetPreviousNode(6u);
  EXPECT_NE(next_node, nullptr);
  EXPECT_EQ(next_node->node_id(), 5u);
}

TEST_F(SemanticTreeTest, NoPreviousNode) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetPreviousNode(0u);
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, NoPreviousLeafNode) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetPreviousNode(3u);
  EXPECT_EQ(next_node, nullptr);
}

TEST_F(SemanticTreeTest, GetPreviousNodeForNonexistentId) {
  SemanticTree::TreeUpdates updates = BuildUpdatesFromFile(kSemanticTreeOddNodesPath);
  EXPECT_TRUE(tree_.Update(std::move(updates)));

  auto next_node = tree_.GetPreviousNode(10u);
  EXPECT_EQ(next_node, nullptr);
}

}  // namespace
}  // namespace accessibility_test
