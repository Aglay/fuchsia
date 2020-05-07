// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/bin/a11y_manager/tests/mocks/mock_semantic_listener.h"

#include <lib/syslog/cpp/macros.h>

namespace accessibility_test {

MockSemanticListener::MockSemanticListener(sys::testing::ComponentContextProvider* context_provider,
                                           fuchsia::ui::views::ViewRef view_ref)
    : context_provider_(context_provider), view_ref_(std::move(view_ref)) {
  context_provider_->ConnectToPublicService(manager_.NewRequest());
  manager_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Cannot connect to ViewManager with status:" << status;
  });
  fidl::InterfaceHandle<SemanticListener> listener_handle;
  bindings_.AddBinding(this, listener_handle.NewRequest());
  manager_->RegisterViewForSemantics(std::move(view_ref_), std::move(listener_handle),
                                     tree_ptr_.NewRequest());
}

void MockSemanticListener::UpdateSemanticNodes(std::vector<Node> nodes) {
  tree_ptr_->UpdateSemanticNodes(std::move(nodes));
}

void MockSemanticListener::DeleteSemanticNodes(std::vector<uint32_t> node_ids) {
  tree_ptr_->DeleteSemanticNodes(std::move(node_ids));
}

void MockSemanticListener::CommitUpdates() {
  tree_ptr_->CommitUpdates([]() {});
}

void MockSemanticListener::SetHitTestResult(std::optional<uint32_t> hit_test_result) {
  hit_test_node_id_ = hit_test_result;
}

void MockSemanticListener::HitTest(::fuchsia::math::PointF local_point, HitTestCallback callback) {
  fuchsia::accessibility::semantics::Hit hit;
  if (hit_test_node_id_) {
    hit.set_node_id(*hit_test_node_id_);
    hit.mutable_path_from_root()->push_back(*hit_test_node_id_);
  }
  callback(std::move(hit));
}

}  // namespace accessibility_test
