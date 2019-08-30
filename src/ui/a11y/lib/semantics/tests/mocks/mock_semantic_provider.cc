// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantic_provider.h"

namespace accessibility_test {

MockSemanticProvider::MockSemanticProvider(
    fuchsia::accessibility::semantics::SemanticsManager* manager,
    fuchsia::ui::views::ViewRef view_ref) {
  manager->RegisterView(std::move(view_ref),
                        action_listener_bindings_.AddBinding(&action_listener_),
                        tree_ptr_.NewRequest());
}

void MockSemanticProvider::UpdateSemanticNodes(
    std::vector<fuchsia::accessibility::semantics::Node> nodes) {
  tree_ptr_->UpdateSemanticNodes(std::move(nodes));
}

void MockSemanticProvider::DeleteSemanticNodes(std::vector<uint32_t> node_ids) {
  tree_ptr_->DeleteSemanticNodes(std::move(node_ids));
}

void MockSemanticProvider::Commit() { tree_ptr_->Commit(); }

void MockSemanticProvider::SetHitTestResult(uint32_t hit_test_result) {
  action_listener_.SetHitTestResult(hit_test_result);
}

}  // namespace accessibility_test
