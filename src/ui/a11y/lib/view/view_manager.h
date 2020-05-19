// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_VIEW_VIEW_MANAGER_H_
#define SRC_UI_A11Y_LIB_VIEW_VIEW_MANAGER_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/vfs/cpp/pseudo_file.h>
#include <zircon/types.h>

#include "src/ui/a11y/lib/annotation/focus_highlight_manager.h"
#include "src/ui/a11y/lib/semantics/semantic_tree_service.h"
#include "src/ui/a11y/lib/semantics/semantics_source.h"
#include "src/ui/a11y/lib/view/view_wrapper.h"

namespace a11y {

// Factory class to build a new Semantic Tree Service.
class SemanticTreeServiceFactory {
 public:
  SemanticTreeServiceFactory() = default;
  virtual ~SemanticTreeServiceFactory() = default;

  virtual std::unique_ptr<SemanticTreeService> NewService(
      zx_koid_t koid, fuchsia::accessibility::semantics::SemanticListenerPtr semantic_listener,
      vfs::PseudoDir* debug_dir, SemanticTreeService::CloseChannelCallback close_channel_callback);
};

// A service to manage producing and consuming of semantics.
//
// Semantic Providers connect to this service to start supplying semantic
// information for a particular View while Semantic Consumers query available
// semantic information managed by this service.
class ViewManager : public fuchsia::accessibility::semantics::SemanticsManager,
                    public SemanticsSource,
                    public FocusHighlightManager {
 public:
  explicit ViewManager(std::unique_ptr<SemanticTreeServiceFactory> factory,
                       std::unique_ptr<ViewSemanticsFactory> view_semantics_factory,
                       std::unique_ptr<AnnotationViewFactoryInterface> annotation_view_factory,
                       sys::ComponentContext* context, vfs::PseudoDir* debug_dir);
  ~ViewManager() override;

  // Function to Enable/Disable Semantics Manager.
  // When Semantics are disabled, all the semantic tree bindings are
  // closed, which deletes all the semantic tree data.
  void SetSemanticsEnabled(bool enabled);

  // |SemanticsSource|
  bool ViewHasSemantics(zx_koid_t view_ref_koid) override;

  // |SemanticsSource|
  const fuchsia::accessibility::semantics::Node* GetSemanticNode(zx_koid_t koid,
                                                                 uint32_t node_id) const override;

  // |SemanticsSource|
  const fuchsia::accessibility::semantics::Node* GetNextNode(zx_koid_t koid,
                                                             uint32_t node_id) const override;

  // |SemanticsSource|
  const fuchsia::accessibility::semantics::Node* GetPreviousNode(zx_koid_t koid,
                                                                 uint32_t node_id) const override;

  // |FocusHighlightManager|
  void ClearHighlight() override;

  // |FocusHighlightManager|
  void UpdateHighlight(SemanticNodeIdentifier newly_highlighted_node) override;

  // |SemanticsSource|
  void ExecuteHitTesting(
      zx_koid_t koid, fuchsia::math::PointF local_point,
      fuchsia::accessibility::semantics::SemanticListener::HitTestCallback callback) override;

  // |SemanticsSource|
  void PerformAccessibilityAction(
      zx_koid_t koid, uint32_t node_id, fuchsia::accessibility::semantics::Action action,
      fuchsia::accessibility::semantics::SemanticListener::OnAccessibilityActionRequestedCallback
          callback) override;

 private:
  // Helper function to retrieve the semantic tree corresponding to |koid|.
  // Returns nullptr if no such tree is found.
  const fxl::WeakPtr<::a11y::SemanticTree> GetTreeByKoid(const zx_koid_t koid) const;

  // Helper function to draw an annotation.
  // Returns true on success, false on failure.
  bool DrawHighlight(SemanticNodeIdentifier newly_highlighted_node);

  // Helper function to clear an existing annotation.
  // Returns true on success, false on failure.
  bool RemoveHighlight();

  // |fuchsia::accessibility::semantics::ViewManager|:
  void RegisterViewForSemantics(
      fuchsia::ui::views::ViewRef view_ref,
      fidl::InterfaceHandle<fuchsia::accessibility::semantics::SemanticListener> handle,
      fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticTree> semantic_tree_request)
      override;

  // ViewSignalHandler is called when ViewRef peer is destroyed. It is
  // responsible for closing the channel and cleaning up the associated SemanticTree.
  void ViewSignalHandler(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                         const zx_packet_signal* signal);

  // |SemanticsSource|
  std::optional<fuchsia::ui::views::ViewRef> ViewRefClone(zx_koid_t view_ref_koid) override;

  std::unordered_map<zx_koid_t, std::unique_ptr<ViewWrapper>> view_wrapper_map_;

  // TODO(36199): Move wait functions inside ViewWrapper.
  std::unordered_map<
      zx_koid_t, std::unique_ptr<async::WaitMethod<ViewManager, &ViewManager::ViewSignalHandler>>>
      wait_map_;

  bool semantics_enabled_ = false;

  std::optional<SemanticNodeIdentifier> highlighted_node_ = std::nullopt;

  std::unique_ptr<SemanticTreeServiceFactory> factory_;

  std::unique_ptr<ViewSemanticsFactory> view_semantics_factory_;

  std::unique_ptr<AnnotationViewFactoryInterface> annotation_view_factory_;

  sys::ComponentContext* context_;

  vfs::PseudoDir* const debug_dir_;
};
}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_VIEW_VIEW_MANAGER_H_
