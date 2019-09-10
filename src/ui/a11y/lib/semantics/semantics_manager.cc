// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/semantics/semantics_manager.h"

#include <lib/syslog/cpp/logger.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include "src/lib/fxl/logging.h"

namespace a11y {

SemanticsManager::SemanticsManager(sys::ComponentContext* startup_context)
    : debug_dir_(startup_context->outgoing()->debug_dir()) {
  FXL_DCHECK(startup_context);
  startup_context->outgoing()->AddPublicService(bindings_.GetHandler(this));
}

SemanticsManager::~SemanticsManager() = default;

void SemanticsManager::RegisterView(
    fuchsia::ui::views::ViewRef view_ref,
    fidl::InterfaceHandle<fuchsia::accessibility::semantics::SemanticActionListener> handle,
    fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticTree> semantic_tree_request) {
  // During View Registration, Semantics manager will ignore enabled flag, to
  // avoid race condition with Semantic Provider(flutter/chrome, etc) since both
  // semantic provider and semantics manager will be notified together about a
  // change in settings.
  // Semantics Manager clears out old bindings when Screen Reader is
  // disabled, and will rely on clients to make sure they only try to register
  // views when screen reader is enabled.

  fuchsia::accessibility::semantics::SemanticActionListenerPtr action_listener = handle.Bind();
  // TODO(MI4-1736): Log View information in below error handler, once ViewRef
  // support is added.
  action_listener.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Semantic Provider disconnected with status: "
                   << zx_status_get_string(status);
  });
  fuchsia::accessibility::semantics::SemanticListenerPtr semantic_listener;

  CompleteSemanticRegistration(std::move(view_ref), std::move(action_listener),
                               std::move(semantic_listener), std::move(semantic_tree_request));
}

void SemanticsManager::RegisterViewForSemantics(
    fuchsia::ui::views::ViewRef view_ref,
    fidl::InterfaceHandle<fuchsia::accessibility::semantics::SemanticListener> handle,
    fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticTree> semantic_tree_request) {
  // During View Registration, Semantics manager will ignore enabled flag, to
  // avoid race condition with Semantic Provider(flutter/chrome, etc) since both
  // semantic provider and semantics manager will be notified together about a
  // change in settings.
  // Semantics Manager clears out old bindings when Screen Reader is
  // disabled, and will rely on clients to make sure they only try to register
  // views when screen reader is enabled.

  fuchsia::accessibility::semantics::SemanticListenerPtr semantic_listener = handle.Bind();
  semantic_listener.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Semantic Provider disconnected with status: "
                   << zx_status_get_string(status);
  });
  // Semantic Action Listener will be deleted in subsequent cl's.
  fuchsia::accessibility::semantics::SemanticActionListenerPtr action_listener;

  CompleteSemanticRegistration(std::move(view_ref), std::move(action_listener),
                               std::move(semantic_listener), std::move(semantic_tree_request));
}

void SemanticsManager::CompleteSemanticRegistration(
    fuchsia::ui::views::ViewRef view_ref,
    fuchsia::accessibility::semantics::SemanticActionListenerPtr action_listener,
    fuchsia::accessibility::semantics::SemanticListenerPtr semantic_listener,
    fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticTree> semantic_tree_request) {
  auto semantic_tree = std::make_unique<SemanticTree>(
      std::move(view_ref), std::move(action_listener), std::move(semantic_listener), debug_dir_,
      /*commit_error_callback=*/[this](zx_koid_t koid) { CloseChannel(koid); });

  semantic_tree_bindings_.AddBinding(std::move(semantic_tree), std::move(semantic_tree_request));
}

fuchsia::accessibility::semantics::NodePtr SemanticsManager::GetAccessibilityNode(
    const fuchsia::ui::views::ViewRef& view_ref, const int32_t node_id) {
  for (auto& binding : semantic_tree_bindings_.bindings()) {
    if (binding->impl()->IsSameView(view_ref)) {
      return binding->impl()->GetAccessibilityNode(node_id);
    }
  }
  return nullptr;
}

fuchsia::accessibility::semantics::NodePtr SemanticsManager::GetAccessibilityNodeByKoid(
    const zx_koid_t koid, const int32_t node_id) {
  for (auto& binding : semantic_tree_bindings_.bindings()) {
    if (binding->impl()->IsSameKoid(koid)) {
      return binding->impl()->GetAccessibilityNode(node_id);
    }
  }
  return nullptr;
}

void SemanticsManager::SetSemanticsManagerEnabled(bool enabled) {
  if ((enabled_ != enabled) && !enabled) {
    FX_LOGS(INFO) << "Resetting SemanticsTree since SemanticsManager is disabled.";
    bindings_.CloseAll();
    semantic_tree_bindings_.CloseAll();
  }
  enabled_ = enabled;
}

void SemanticsManager::PerformHitTesting(
    zx_koid_t koid, ::fuchsia::math::PointF local_point,
    fuchsia::accessibility::semantics::SemanticActionListener::HitTestCallback callback) {
  for (auto& binding : semantic_tree_bindings_.bindings()) {
    if (binding->impl()->IsSameKoid(koid)) {
      return binding->impl()->PerformHitTesting(local_point, std::move(callback));
    }
  }

  FX_LOGS(INFO) << "Given KOID(" << koid << ") doesn't match any existing ViewRef's koid.";
}

void SemanticsManager::CloseChannel(zx_koid_t koid) {
  for (auto& binding : semantic_tree_bindings_.bindings()) {
    if (binding->impl()->IsSameKoid(koid)) {
      semantic_tree_bindings_.RemoveBinding(binding->impl());
    }
  }
}

}  // namespace a11y
