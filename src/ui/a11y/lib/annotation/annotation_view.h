
// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_ANNOTATION_ANNOTATION_VIEW_H_
#define SRC_UI_A11Y_LIB_ANNOTATION_ANNOTATION_VIEW_H_

#include <fuchsia/ui/annotation/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <memory>

#include "src/ui/a11y/lib/view/view_manager.h"

namespace a11y {

// The AnnotationView class enables the fuchsia accessibility manager to draw annotations over
// client views.
class AnnotationView : public fuchsia::ui::scenic::SessionListener {
 public:
  // Stores state of annotation view.
  struct AnnotationViewState {
    // True after annotation view has been registered via the scenic annotation registry API.
    bool annotation_view_registered = false;

    // True after the annotation view's node tree has been set up.
    bool tree_initialized = false;

    // True if annotations are currently displayed, and false otherwise.
    bool has_annotations = false;

    // Node id for currently annotated node, if any.
    bool annotated_node_id = 0;
  };

  explicit AnnotationView(sys::ComponentContext* component_context,
                          a11y::ViewManager* semantics_manager, zx_koid_t client_view_koid);

  ~AnnotationView() = default;

  // NOTE: Callers MUST call InitializeView() before calling HighlightNode().
  // Creates an annotation view in session private to this view class and a corresponding view
  // holder in scenic, and then initializes the view's node structure to allow callers to annotate
  // the corresonding view.
  void InitializeView(fuchsia::ui::views::ViewRef client_view_ref);

  // Draws a bounding box around the node with id node_id.
  void HighlightNode(uint32_t node_id);

  // Hides annotation view contents by detaching the subtree containing the annotations from the
  // view.
  void DetachViewContents();

 private:
  // Helper function to draw four rectangles corresponding to the top, bottom, left, and right edges
  // of a node's bounding box.
  void DrawHighlight(const fuchsia::accessibility::semantics::Node* node);

  // Helper function to build a list of commands to enqueue.
  static void PushCommand(std::vector<fuchsia::ui::scenic::Command>* cmds,
                          fuchsia::ui::gfx::Command cmd);

  // Scenic error handler.
  void OnScenicError(std::string error) override {}

  // Scenic event handler.
  void OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events) override;

  // Helper function to handle gfx events (e.g. switching or resizing view).
  void HandleGfxEvent(const fuchsia::ui::gfx::Event& event);

  // Stores state of annotation view
  AnnotationViewState state_;

  // View manager object used to retrieve relevant semantic node info.
  a11y::ViewManager* view_manager_;

  // KOID of the client view this annotation view is used to annotate.
  zx_koid_t client_view_koid_;

  // Scenic session listener.
  fidl::Binding<fuchsia::ui::scenic::SessionListener> session_listener_binding_;

  // Scenic session interface.
  fuchsia::ui::scenic::SessionPtr session_;

  // Interface between a11y manager and Scenic annotation registry to register annotation
  // viewholder with Scenic.
  fuchsia::ui::annotation::RegistryPtr annotation_registry_;
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_ANNOTATION_ANNOTATION_VIEW_H_
