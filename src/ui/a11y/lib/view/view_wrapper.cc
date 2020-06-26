// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "view_wrapper.h"

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <lib/async/default.h>
#include <lib/syslog/cpp/macros.h>

#include <stack>

namespace a11y {

ViewWrapper::ViewWrapper(fuchsia::ui::views::ViewRef view_ref,
                         std::unique_ptr<ViewSemantics> view_semantics,
                         std::unique_ptr<AnnotationViewInterface> annotation_view)
    : view_ref_(std::move(view_ref)),
      view_semantics_(std::move(view_semantics)),
      annotation_view_(std::move(annotation_view)) {}

void ViewWrapper::EnableSemanticUpdates(bool enabled) {
  view_semantics_->EnableSemanticUpdates(enabled);
}

fxl::WeakPtr<::a11y::SemanticTree> ViewWrapper::GetTree() { return view_semantics_->GetTree(); }

fuchsia::ui::views::ViewRef ViewWrapper::ViewRefClone() const { return Clone(view_ref_); }

void ViewWrapper::HighlightNode(uint32_t node_id) {
  auto tree_weak_ptr = GetTree();

  if (!tree_weak_ptr) {
    FX_LOGS(ERROR) << "ViewWrapper::DrawHighlight: Invalid tree pointer";
    return;
  }

  auto annotated_node = tree_weak_ptr->GetNode(node_id);

  if (!annotated_node) {
    FX_LOGS(ERROR) << "ViewWrapper::DrawHighlight: No node found with id: " << node_id;
    return;
  }

  // Compute the translation and scaling vectors for the node's bounding box.
  // Each node can supply a 4x4 transform matrix of the form:
  // [ Sx   0    0    Tx ]
  // [ 0    Sy   0    Ty ]
  // [ 0    0    Sz   Tz ]
  // [ 0    0    0    1  ]
  //
  // Here, Sx, Sy, and Sz are the scale coefficients on the x, y, and z axes,
  // respectively. Tx, Ty, and Tz are the x, y, and z components of translation,
  // respectively.
  //
  // In order to comput the transform matrix from the root node's coordinate
  // space to the focused node's coordinate space, we can simply compute the
  // cross product of the focused node's ancestors' transform matrices,
  // beginning at the minimum-depth non-root ancestor (the root does not have a
  // parent, so it does not need a transform):
  //
  // [Focused node transform] = [depth 1 ancestor transform] x [depth 2 ancestor
  // transform] x ... x [parent transform] x [focused node transform]
  //
  // Assuming that the transform matrices are of the form described above, we
  // don't actually need to do the full matrix multiplications. The matrix
  // arithmetic will work out such that Sx = Sx1 * Sx2 * ..., Sy = ..., Sz = ...
  // and Tx = (Sx2 * Sx3 * ...)(Tx1) + (Sx3 * Sx4 * ...)(Tx2) + ....
  //
  // The resulting transform will be of the same form as described above. Using
  // this matrix, we can simply extract the scaling and translation vectors
  // required by scenic: (Sx, Sy, Sz) and (Tx, Ty, Tz), respectively.
  uint32_t current_node_id = annotated_node->node_id();
  std::stack<const fuchsia::accessibility::semantics::Node*> nodes_to_visit;
  while (current_node_id != 0) {
    auto current_node = tree_weak_ptr->GetNode(current_node_id);
    FX_DCHECK(current_node);

    if (current_node->has_transform()) {
      nodes_to_visit.push(current_node);
    }

    auto parent_node = tree_weak_ptr->GetParentNode(current_node_id);
    FX_DCHECK(parent_node);
    current_node_id = parent_node->node_id();
  }

  std::array<float, 3> scale_vector({1.0f, 1.0f, 1.0f});
  std::array<float, 3> translation_vector({0.f, 0.f, 0.f});
  while (!nodes_to_visit.empty()) {
    auto current_node = nodes_to_visit.top();
    nodes_to_visit.pop();

    auto local_transform = current_node->transform().matrix;
    scale_vector[0] *= local_transform[0];
    scale_vector[1] *= local_transform[5];
    scale_vector[2] *= local_transform[10];

    translation_vector[0] = (local_transform[0] * translation_vector[0]) + local_transform[12];
    translation_vector[1] = (local_transform[5] * translation_vector[1]) + local_transform[13];
    translation_vector[2] = (local_transform[10] * translation_vector[2]) + local_transform[14];
  }

  auto bounding_box = annotated_node->location();
  annotation_view_->DrawHighlight(bounding_box, scale_vector, translation_vector);
}

void ViewWrapper::ClearHighlights() { annotation_view_->DetachViewContents(); }

}  // namespace a11y
