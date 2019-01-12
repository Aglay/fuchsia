// Copyright 2015 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/ui/view_manager/view_registry.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/ui/input/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "garnet/bin/ui/view_manager/view_impl.h"
#include "garnet/bin/ui/view_manager/view_tree_impl.h"
#include "garnet/public/lib/escher/util/type_utils.h"
#include "lib/component/cpp/connect.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/ui/scenic/cpp/commands.h"
#include "lib/ui/scenic/cpp/resources.h"
#include "lib/ui/views/cpp/formatting.h"

namespace view_manager {
namespace {

class SnapshotCallbackImpl : public fuchsia::ui::gfx::SnapshotCallbackHACK {
 private:
  fit::function<void(::fuchsia::mem::Buffer)> callback_;
  fidl::Binding<::fuchsia::ui::gfx::SnapshotCallbackHACK> binding_;
  fit::function<void()> clear_fn_;

 public:
  explicit SnapshotCallbackImpl(
      fidl::InterfaceRequest<fuchsia::ui::gfx::SnapshotCallbackHACK> request,
      fit::function<void(::fuchsia::mem::Buffer)> callback)
      : callback_(std::move(callback)), binding_(this, std::move(request)) {}
  ~SnapshotCallbackImpl() {}
  void SetClear(fit::function<void()> clear_fn) {
    clear_fn_ = std::move(clear_fn);
  }

  virtual void OnData(::fuchsia::mem::Buffer data) override {
    callback_(std::move(data));
    if (clear_fn_)
      clear_fn_();
  }
};

bool Validate(const ::fuchsia::ui::viewsv1::ViewLayout& value) {
  return value.size.width >= 0 && value.size.height >= 0;
}

bool Validate(const ::fuchsia::ui::viewsv1::ViewProperties& value) {
  if (value.view_layout && !Validate(*value.view_layout))
    return false;
  return true;
}

// Returns true if the properties are valid and are sufficient for
// operating the view tree.
bool IsComplete(const ::fuchsia::ui::viewsv1::ViewProperties& value) {
  return Validate(value) && value.view_layout;
}

void ApplyOverrides(::fuchsia::ui::viewsv1::ViewProperties* value,
                    const ::fuchsia::ui::viewsv1::ViewProperties* overrides) {
  if (!overrides)
    return;
  if (overrides->view_layout)
    *value->view_layout = *overrides->view_layout;
}

std::string SanitizeLabel(fidl::StringPtr label) {
  return label.get().substr(0, ::fuchsia::ui::viewsv1::kLabelMaxLength);
}

bool Equals(const ::fuchsia::ui::viewsv1::ViewPropertiesPtr& a,
            const ::fuchsia::ui::viewsv1::ViewPropertiesPtr& b) {
  if (!a || !b)
    return !a && !b;
  return *a == *b;
}

}  // namespace

ViewRegistry::ViewRegistry(component::StartupContext* startup_context)
    : startup_context_(startup_context),
      scenic_(startup_context_
                  ->ConnectToEnvironmentService<fuchsia::ui::scenic::Scenic>()),
      session_(scenic_.get()),
      weak_factory_(this) {
  // TODO(MZ-128): Register session listener and destroy views if their
  // content nodes become unavailable.

  scenic_.set_error_handler([](zx_status_t error) {
    FXL_LOG(ERROR) << "Exiting due to scene manager connection error.";
    exit(1);
  });

  session_.set_error_handler([](zx_status_t error) {
    FXL_LOG(ERROR) << "Exiting due to session connection error.";
    exit(1);
  });
}

ViewRegistry::~ViewRegistry() {}

void ViewRegistry::GetScenic(
    fidl::InterfaceRequest<fuchsia::ui::scenic::Scenic> scenic_request) {
  // TODO(jeffbrown): We should have a better way to duplicate the
  // SceneManager connection without going back out through the environment.
  startup_context_->ConnectToEnvironmentService(std::move(scenic_request));
}

// CREATE / DESTROY VIEWS

void ViewRegistry::CreateView(
    fidl::InterfaceRequest<::fuchsia::ui::viewsv1::View> view_request,
    zx::eventpair view_token,
    ::fuchsia::ui::viewsv1::ViewListenerPtr view_listener,
    zx::eventpair parent_export_token, fidl::StringPtr label) {
  FXL_DCHECK(view_request.is_valid());
  FXL_DCHECK(view_token);
  FXL_DCHECK(view_listener);
  FXL_DCHECK(parent_export_token);

  uint32_t view_id = next_view_id_value_++;
  FXL_CHECK(view_id);
  FXL_CHECK(!FindView(view_id));

  // Create the state and bind the interfaces to it.
  ViewLinker::ImportLink view_owner_link =
      view_linker_.CreateImport(std::move(view_token), this);
  auto view_state = std::make_unique<ViewState>(
      this, view_id, std::move(view_request), std::move(view_listener),
      &session_, SanitizeLabel(label));

  // Export a node which represents the view's attachment point.
  view_state->top_node().Export(std::move(parent_export_token));
  view_state->top_node().SetTag(view_state->view_token());
  view_state->top_node().SetLabel(view_state->FormattedLabel());

  // TODO(MZ-371): Avoid Z-fighting by introducing a smidgen of elevation
  // between each view and its embedded sub-views.  This is not a long-term fix.
  view_state->top_node().SetTranslation(0.f, 0.f, 0.1f);
  SchedulePresentSession();

  // Begin tracking the view, and bind it to the owner link.  Binding may cause
  // the ViewStub to be attached, so we make sure to begin tracking the view in
  // the map beforehand.
  ViewState* view_state_ptr = view_state.get();
  views_by_token_.emplace(view_id, std::move(view_state));
  view_state_ptr->BindOwner(std::move(view_owner_link));
  FXL_VLOG(1) << "CreateView: view=" << view_state_ptr;
}

void ViewRegistry::OnViewDied(ViewState* view_state,
                              const std::string& reason) {
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));
  FXL_VLOG(1) << "OnViewDied: view=" << view_state << ", reason=" << reason;

  UnregisterView(view_state);
}

void ViewRegistry::UnregisterView(ViewState* view_state) {
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));
  FXL_VLOG(1) << "UnregisterView: view=" << view_state;

  if (ViewStub* view_stub = view_state->view_stub()) {
    view_stub->ReleaseView();
  }
  UnregisterChildren(view_state);

  // Remove the view's content node from the session.
  view_state->top_node().Detach();
  SchedulePresentSession();

  // Remove from registry.
  views_by_token_.erase(view_state->view_token());
}

// CREATE / DESTROY VIEW TREES

void ViewRegistry::CreateViewTree(
    fidl::InterfaceRequest<::fuchsia::ui::viewsv1::ViewTree> view_tree_request,
    ::fuchsia::ui::viewsv1::ViewTreeListenerPtr view_tree_listener,
    fidl::StringPtr label) {
  FXL_DCHECK(view_tree_request.is_valid());
  FXL_DCHECK(view_tree_listener);

  ::fuchsia::ui::viewsv1::ViewTreeToken view_tree_token;
  view_tree_token.value = next_view_tree_token_value_++;
  FXL_CHECK(view_tree_token.value);
  FXL_CHECK(!FindViewTree(view_tree_token.value));

  // Create the state and bind the interfaces to it.
  auto tree_state = std::make_unique<ViewTreeState>(
      this, view_tree_token, std::move(view_tree_request),
      std::move(view_tree_listener), SanitizeLabel(label));

  // Add to registry.
  ViewTreeState* tree_state_ptr = tree_state.get();
  view_trees_by_token_.emplace(tree_state->view_tree_token().value,
                               std::move(tree_state));
  FXL_VLOG(1) << "CreateViewTree: tree=" << tree_state_ptr;
}

void ViewRegistry::OnViewTreeDied(ViewTreeState* tree_state,
                                  const std::string& reason) {
  FXL_DCHECK(IsViewTreeStateRegisteredDebug(tree_state));
  FXL_VLOG(1) << "OnViewTreeDied: tree=" << tree_state << ", reason=" << reason;

  UnregisterViewTree(tree_state);
}

void ViewRegistry::UnregisterViewTree(ViewTreeState* tree_state) {
  FXL_DCHECK(IsViewTreeStateRegisteredDebug(tree_state));
  FXL_VLOG(1) << "UnregisterViewTree: tree=" << tree_state;

  UnregisterChildren(tree_state);

  // Remove from registry.
  view_trees_by_token_.erase(tree_state->view_tree_token().value);
}

// LIFETIME

void ViewRegistry::UnregisterViewContainer(
    ViewContainerState* container_state) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));

  ViewState* view_state = container_state->AsViewState();
  if (view_state)
    UnregisterView(view_state);
  else
    UnregisterViewTree(container_state->AsViewTreeState());
}

void ViewRegistry::UnregisterViewStub(std::unique_ptr<ViewStub> view_stub) {
  FXL_DCHECK(view_stub);

  ViewState* view_state = view_stub->ReleaseView();
  if (view_state)
    UnregisterView(view_state);

  ReleaseViewStubChildHost(view_stub.get());
}

void ViewRegistry::UnregisterChildren(ViewContainerState* container_state) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));

  // Recursively unregister all children since they will become unowned
  // at this point taking care to unlink each one before its unregistration.
  for (auto& child : container_state->UnlinkAllChildren())
    UnregisterViewStub(std::move(child));
}

void ViewRegistry::ReleaseViewStubChildHost(ViewStub* view_stub) {
  view_stub->ReleaseHost();
  SchedulePresentSession();
}

// TREE MANIPULATION

void ViewRegistry::AddChild(ViewContainerState* container_state,
                            uint32_t child_key, zx::eventpair view_holder_token,
                            zx::eventpair host_import_token) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));
  FXL_DCHECK(view_holder_token);
  FXL_DCHECK(host_import_token);
  FXL_VLOG(1) << "AddChild: container=" << container_state
              << ", child_key=" << child_key;

  // Ensure there are no other children with the same key.
  if (container_state->children().find(child_key) !=
      container_state->children().end()) {
    FXL_LOG(ERROR) << "Attempted to add a child with a duplicate key: "
                   << "container=" << container_state
                   << ", child_key=" << child_key;
    UnregisterViewContainer(container_state);
    return;
  }

  // If this is a view tree, ensure it only has one root.
  ViewTreeState* view_tree_state = container_state->AsViewTreeState();
  if (view_tree_state && !container_state->children().empty()) {
    FXL_LOG(ERROR) << "Attempted to add a second child to a view tree: "
                   << "container=" << container_state
                   << ", child_key=" << child_key;
    UnregisterViewContainer(container_state);
    return;
  }

  // Add a stub, pending resolution of the view owner.
  // Assuming the stub isn't removed prematurely, |OnViewResolved| will be
  // called asynchronously with the result of the resolution.
  ViewLinker::ExportLink view_link =
      view_linker_.CreateExport(std::move(view_holder_token), this);
  container_state->LinkChild(child_key, std::unique_ptr<ViewStub>(new ViewStub(
                                            this, std::move(view_link),
                                            std::move(host_import_token))));
}

void ViewRegistry::RemoveChild(ViewContainerState* container_state,
                               uint32_t child_key,
                               zx::eventpair transferred_view_holder_token) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));
  FXL_VLOG(1) << "RemoveChild: container=" << container_state
              << ", child_key=" << child_key;

  // Ensure the child key exists in the container.
  auto child_it = container_state->children().find(child_key);
  if (child_it == container_state->children().end()) {
    FXL_LOG(ERROR) << "Attempted to remove a child with an invalid key: "
                   << "container=" << container_state
                   << ", child_key=" << child_key;
    UnregisterViewContainer(container_state);
    return;
  }

  // Unlink the child from its container.
  TransferOrUnregisterViewStub(container_state->UnlinkChild(child_key),
                               std::move(transferred_view_holder_token));
}

void ViewRegistry::SetChildProperties(
    ViewContainerState* container_state, uint32_t child_key,
    ::fuchsia::ui::viewsv1::ViewPropertiesPtr child_properties) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));
  FXL_VLOG(1) << "SetChildProperties: container=" << container_state
              << ", child_key=" << child_key
              << ", child_properties=" << child_properties;

  // Check whether the properties are well-formed.
  if (child_properties && !Validate(*child_properties)) {
    FXL_LOG(ERROR) << "Attempted to set invalid child view properties: "
                   << "container=" << container_state
                   << ", child_key=" << child_key
                   << ", child_properties=" << child_properties;
    UnregisterViewContainer(container_state);
    return;
  }

  // Check whether the child key exists in the container.
  auto child_it = container_state->children().find(child_key);
  if (child_it == container_state->children().end()) {
    FXL_LOG(ERROR) << "Attempted to modify child with an invalid key: "
                   << "container=" << container_state
                   << ", child_key=" << child_key
                   << ", child_properties=" << child_properties;
    UnregisterViewContainer(container_state);
    return;
  }

  // Immediately discard requests on unavailable views.
  ViewStub* child_stub = child_it->second.get();
  if (child_stub->is_unavailable())
    return;

  // Store the updated properties specified by the container if changed.
  if (Equals(child_properties, child_stub->properties()))
    return;

  // Apply the change.
  child_stub->SetProperties(std::move(child_properties), &session_);
  if (child_stub->state()) {
    InvalidateView(child_stub->state(),
                   ViewState::INVALIDATION_PROPERTIES_CHANGED);
  }
}

void ViewRegistry::RequestSnapshotHACK(
    ViewContainerState* container_state, uint32_t child_key,
    fit::function<void(::fuchsia::mem::Buffer)> callback) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));

  // Check whether the child key exists in the container.
  auto child_it = container_state->children().find(child_key);
  if (child_it == container_state->children().end()) {
    FXL_LOG(ERROR) << "Attempted to modify child with an invalid key: "
                   << "container=" << container_state
                   << ", child_key=" << child_key;
    UnregisterViewContainer(container_state);
    // TODO(SCN-978): Return an error to the caller for invalid data.
    callback(fuchsia::mem::Buffer{});
    return;
  }

  // Immediately discard requests on unavailable views.
  ViewStub* child_stub = child_it->second.get();
  if (child_stub->is_unavailable() || child_stub->is_pending()) {
    FXL_VLOG(1) << "RequestSnapshot called for view that is currently "
                << (child_stub->is_unavailable() ? "unavailable" : "pending");
    // TODO(SCN-978): Return an error to the caller for invalid data.
    callback(fuchsia::mem::Buffer{});
    return;
  }

  fuchsia::ui::gfx::SnapshotCallbackHACKPtr snapshot_callback;
  auto snapshot_callback_impl = std::make_shared<SnapshotCallbackImpl>(
      snapshot_callback.NewRequest(), std::move(callback));
  snapshot_callback_impl->SetClear([this, snapshot_callback_impl]() {
    snapshot_bindings_.remove(snapshot_callback_impl);
  });
  snapshot_bindings_.push_back(std::move(snapshot_callback_impl));

  // Snapshot the child.
  child_stub->state()->top_node().Snapshot(std::move(snapshot_callback));
  SchedulePresentSession();
}

void ViewRegistry::SendSizeChangeHintHACK(ViewContainerState* container_state,
                                          uint32_t child_key,
                                          float width_change_factor,
                                          float height_change_factor) {
  FXL_DCHECK(IsViewContainerStateRegisteredDebug(container_state));
  FXL_VLOG(1) << "SendSizeChangeHintHACK: container=" << container_state
              << ", width_change_factor=" << width_change_factor
              << ", height_change_factor=" << height_change_factor << "}";

  // Check whether the child key exists in the container.
  auto child_it = container_state->children().find(child_key);
  if (child_it == container_state->children().end()) {
    FXL_LOG(ERROR) << "Attempted to modify child with an invalid key: "
                   << "container=" << container_state
                   << ", child_key=" << child_key;
    UnregisterViewContainer(container_state);
    return;
  }

  // Immediately discard requests on unavailable views.
  ViewStub* child_stub = child_it->second.get();
  if (child_stub->is_unavailable() || child_stub->is_pending()) {
    FXL_VLOG(1) << "SendSizeChangeHintHACK called for view that is currently "
                << (child_stub->is_unavailable() ? "unavailable" : "pending");
    return;
  }
  FXL_DCHECK(child_stub->state());

  child_stub->state()->top_node().SendSizeChangeHint(width_change_factor,
                                                     height_change_factor);
  SchedulePresentSession();
}

void ViewRegistry::OnViewResolved(ViewStub* view_stub, ViewState* view_state) {
  FXL_DCHECK(view_stub);

  if (view_state)
    AttachResolvedViewAndNotify(view_stub, view_state);
  else
    ReleaseUnavailableViewAndNotify(view_stub);
}

void ViewRegistry::TransferView(ViewState* view_state,
                                zx::eventpair transferred_view_token) {
  FXL_DCHECK(transferred_view_token);

  if (view_state) {
    InvalidateView(view_state, ViewState::INVALIDATION_PARENT_CHANGED);

    // This will cause the view_state to be rebound, and released from the
    // view_stub.
    ViewLinker::ImportLink view_owner_link =
        view_linker_.CreateImport(std::move(transferred_view_token), this);
    view_state->BindOwner(std::move(view_owner_link));
  }
}

void ViewRegistry::AttachResolvedViewAndNotify(ViewStub* view_stub,
                                               ViewState* view_state) {
  FXL_DCHECK(view_stub);
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));
  FXL_VLOG(2) << "AttachViewStubAndNotify: view=" << view_state;

  // Precondition:  The view_state will not have a view_stub attached.
  FXL_CHECK(!view_state->view_stub())
      << "Attempted to attach ViewState " << view_state
      << " that already had a ViewStub";

  // Attach the view's content.
  if (view_stub->container()) {
    view_stub->ImportHostNode(&session_);
    view_stub->host_node()->AddChild(view_state->top_node());
    SchedulePresentSession();

    SendChildAttached(view_stub->container(), view_stub->key(),
                      ::fuchsia::ui::viewsv1::ViewInfo());
  }

  // Attach the view.
  view_stub->AttachView(view_state);
  InvalidateView(view_state, ViewState::INVALIDATION_PARENT_CHANGED);
}

void ViewRegistry::ReleaseUnavailableViewAndNotify(ViewStub* view_stub) {
  FXL_DCHECK(view_stub);
  FXL_VLOG(2) << "ReleaseUnavailableViewAndNotify: key=" << view_stub->key();

  ViewState* view_state = view_stub->ReleaseView();
  FXL_DCHECK(!view_state);

  if (view_stub->container())
    SendChildUnavailable(view_stub->container(), view_stub->key());
}

void ViewRegistry::TransferOrUnregisterViewStub(
    std::unique_ptr<ViewStub> view_stub, zx::eventpair transferred_view_token) {
  FXL_DCHECK(view_stub);

  if (transferred_view_token) {
    ReleaseViewStubChildHost(view_stub.get());

    if (view_stub->state()) {
      ViewState* view_state = view_stub->ReleaseView();
      TransferView(view_state, std::move(transferred_view_token));

      return;
    }

    if (view_stub->is_pending()) {
      FXL_DCHECK(!view_stub->state());

      // Handle transfer of pending view.
      view_stub->TransferViewWhenResolved(std::move(view_stub),
                                          std::move(transferred_view_token));

      return;
    }
  }
  UnregisterViewStub(std::move(view_stub));
}

// INVALIDATION

void ViewRegistry::InvalidateView(ViewState* view_state, uint32_t flags) {
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));
  FXL_VLOG(2) << "InvalidateView: view=" << view_state << ", flags=" << flags;

  view_state->set_invalidation_flags(view_state->invalidation_flags() | flags);
  if (view_state->view_stub() && view_state->view_stub()->tree()) {
    InvalidateViewTree(view_state->view_stub()->tree(),
                       ViewTreeState::INVALIDATION_VIEWS_INVALIDATED);
  }
}

void ViewRegistry::InvalidateViewTree(ViewTreeState* tree_state,
                                      uint32_t flags) {
  FXL_DCHECK(IsViewTreeStateRegisteredDebug(tree_state));
  FXL_VLOG(2) << "InvalidateViewTree: tree=" << tree_state
              << ", flags=" << flags;

  tree_state->set_invalidation_flags(tree_state->invalidation_flags() | flags);
  ScheduleTraversal();
}

void ViewRegistry::ScheduleTraversal() {
  if (!traversal_scheduled_) {
    traversal_scheduled_ = true;
    async::PostTask(async_get_default_dispatcher(),
                    [weak = weak_factory_.GetWeakPtr()] {
                      if (weak)
                        weak->Traverse();
                    });
  }
}

void ViewRegistry::Traverse() {
  FXL_DCHECK(traversal_scheduled_);

  traversal_scheduled_ = false;
  for (const auto& pair : view_trees_by_token_)
    TraverseViewTree(pair.second.get());
}

void ViewRegistry::TraverseViewTree(ViewTreeState* tree_state) {
  FXL_DCHECK(IsViewTreeStateRegisteredDebug(tree_state));
  FXL_VLOG(2) << "TraverseViewTree: tree=" << tree_state
              << ", invalidation_flags=" << tree_state->invalidation_flags();

  uint32_t flags = tree_state->invalidation_flags();

  if (flags & ViewTreeState::INVALIDATION_VIEWS_INVALIDATED) {
    ViewStub* root_stub = tree_state->GetRoot();
    if (root_stub && root_stub->state())
      TraverseView(root_stub->state(), false);
  }

  tree_state->set_invalidation_flags(0u);
}

void ViewRegistry::TraverseView(ViewState* view_state,
                                bool parent_properties_changed) {
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));
  FXL_VLOG(2) << "TraverseView: view=" << view_state
              << ", parent_properties_changed=" << parent_properties_changed
              << ", invalidation_flags=" << view_state->invalidation_flags();

  uint32_t flags = view_state->invalidation_flags();

  // Update view properties.
  bool view_properties_changed = false;
  if (parent_properties_changed ||
      (flags & (ViewState::INVALIDATION_PROPERTIES_CHANGED |
                ViewState::INVALIDATION_PARENT_CHANGED))) {
    ::fuchsia::ui::viewsv1::ViewPropertiesPtr properties =
        ResolveViewProperties(view_state);
    if (properties) {
      if (!view_state->issued_properties() ||
          !Equals(properties, view_state->issued_properties())) {
        view_state->IssueProperties(std::move(properties));
        view_properties_changed = true;
      }
    }
    flags &= ~(ViewState::INVALIDATION_PROPERTIES_CHANGED |
               ViewState::INVALIDATION_PARENT_CHANGED);
  }

  // If we don't have view properties yet then we cannot pursue traversals
  // any further.
  if (!view_state->issued_properties()) {
    FXL_VLOG(2) << "View has no valid properties: view=" << view_state;
    view_state->set_invalidation_flags(flags);
    return;
  }

  // Deliver property change event if needed.
  bool send_properties = view_properties_changed ||
                         (flags & ViewState::INVALIDATION_RESEND_PROPERTIES);
  if (send_properties) {
    if (!(flags & ViewState::INVALIDATION_IN_PROGRESS)) {
      ::fuchsia::ui::viewsv1::ViewProperties cloned_properties;
      view_state->issued_properties()->Clone(&cloned_properties);
      SendPropertiesChanged(view_state, std::move(cloned_properties));
      flags = ViewState::INVALIDATION_IN_PROGRESS;
    } else {
      FXL_VLOG(2) << "View invalidation stalled awaiting response: view="
                  << view_state;
      if (send_properties)
        flags |= ViewState::INVALIDATION_RESEND_PROPERTIES;
      flags |= ViewState::INVALIDATION_STALLED;
    }
  }
  view_state->set_invalidation_flags(flags);

  // TODO(jeffbrown): Optimize propagation.
  // This should defer traversal of the rest of the subtree until the view
  // flushes its container or a timeout expires.  We will need to be careful
  // to ensure that we completely process one traversal before starting the
  // next one and we'll have to retain some state.  The same behavior should
  // be applied when the parent's own properties change (assuming that it is
  // likely to want to resize its children, unless it says otherwise somehow).

  // Traverse all children.
  for (const auto& pair : view_state->children()) {
    ViewState* child_state = pair.second->state();
    if (child_state)
      TraverseView(pair.second->state(), view_properties_changed);
  }
}

::fuchsia::ui::viewsv1::ViewPropertiesPtr ViewRegistry::ResolveViewProperties(
    ViewState* view_state) {
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));

  ViewStub* view_stub = view_state->view_stub();
  if (!view_stub || !view_stub->properties())
    return nullptr;

  if (view_stub->parent()) {
    if (!view_stub->parent()->issued_properties())
      return nullptr;
    auto properties = ::fuchsia::ui::viewsv1::ViewProperties::New();
    view_stub->parent()->issued_properties()->Clone(properties.get());
    ApplyOverrides(properties.get(), view_stub->properties().get());
    return properties;
  } else if (view_stub->is_root_of_tree()) {
    if (!view_stub->properties() || !IsComplete(*view_stub->properties())) {
      FXL_VLOG(2) << "View tree properties are incomplete: root=" << view_state
                  << ", properties=" << view_stub->properties();
      return nullptr;
    }
    auto cloned_properties = ::fuchsia::ui::viewsv1::ViewProperties::New();
    view_stub->properties()->Clone(cloned_properties.get());
    return cloned_properties;
  } else {
    return nullptr;
  }
}

void ViewRegistry::SchedulePresentSession() {
  if (!present_session_scheduled_) {
    present_session_scheduled_ = true;
    async::PostTask(async_get_default_dispatcher(),
                    [weak = weak_factory_.GetWeakPtr()] {
                      if (weak)
                        weak->PresentSession();
                    });
  }
}

void ViewRegistry::PresentSession() {
  FXL_DCHECK(present_session_scheduled_);

  present_session_scheduled_ = false;
  session_.Present(0, [this](fuchsia::images::PresentationInfo info) {});
}

// VIEW AND VIEW TREE SERVICE PROVIDERS

void ViewRegistry::ConnectToViewService(ViewState* view_state,
                                        const fidl::StringPtr& service_name,
                                        zx::channel client_handle) {
  FXL_DCHECK(IsViewStateRegisteredDebug(view_state));
}

void ViewRegistry::ConnectToViewTreeService(ViewTreeState* tree_state,
                                            const fidl::StringPtr& service_name,
                                            zx::channel client_handle) {
  FXL_DCHECK(IsViewTreeStateRegisteredDebug(tree_state));
}

// EXTERNAL SIGNALING

void ViewRegistry::SendPropertiesChanged(
    ViewState* view_state, ::fuchsia::ui::viewsv1::ViewProperties properties) {
  FXL_DCHECK(view_state);
  FXL_DCHECK(view_state->view_listener());

  FXL_VLOG(1) << "SendPropertiesChanged: view_state=" << view_state
              << ", properties=" << properties;

  // It's safe to capture the view state because the ViewListener is closed
  // before the view state is destroyed so we will only receive the callback
  // if the view state is still alive.
  view_state->view_listener()->OnPropertiesChanged(
      std::move(properties), [this, view_state] {
        uint32_t old_flags = view_state->invalidation_flags();
        FXL_DCHECK(old_flags & ViewState::INVALIDATION_IN_PROGRESS);

        view_state->set_invalidation_flags(
            old_flags & ~(ViewState::INVALIDATION_IN_PROGRESS |
                          ViewState::INVALIDATION_STALLED));

        if (old_flags & ViewState::INVALIDATION_STALLED) {
          FXL_VLOG(2) << "View recovered from stalled invalidation: view_state="
                      << view_state;
          InvalidateView(view_state, 0u);
        }
      });
}

void ViewRegistry::SendChildAttached(
    ViewContainerState* container_state, uint32_t child_key,
    ::fuchsia::ui::viewsv1::ViewInfo child_view_info) {
  FXL_DCHECK(container_state);

  if (!container_state->view_container_listener())
    return;

  // TODO: Detect ANRs
  FXL_VLOG(1) << "SendChildAttached: container_state=" << container_state
              << ", child_key=" << child_key
              << ", child_view_info=" << child_view_info;
  container_state->view_container_listener()->OnChildAttached(
      child_key, child_view_info, [] {});
}

void ViewRegistry::SendChildUnavailable(ViewContainerState* container_state,
                                        uint32_t child_key) {
  FXL_DCHECK(container_state);

  if (!container_state->view_container_listener())
    return;

  // TODO: Detect ANRs
  FXL_VLOG(1) << "SendChildUnavailable: container=" << container_state
              << ", child_key=" << child_key;
  container_state->view_container_listener()->OnChildUnavailable(child_key,
                                                                 [] {});
}

// SNAPSHOT
void ViewRegistry::TakeSnapshot(
    uint64_t view_koid, fit::function<void(::fuchsia::mem::Buffer)> callback) {
  auto view_state = static_cast<ViewState*>(view_linker_.GetImport(view_koid));
  if (view_koid > 0 && !view_state) {
    // TODO(SCN-978): Did not find the view for the view koid, return error.
    callback(fuchsia::mem::Buffer{});
    return;
  }

  fuchsia::ui::gfx::SnapshotCallbackHACKPtr snapshot_callback;
  auto snapshot_callback_impl = std::make_shared<SnapshotCallbackImpl>(
      snapshot_callback.NewRequest(), std::move(callback));
  snapshot_callback_impl->SetClear([this, snapshot_callback_impl]() {
    snapshot_bindings_.remove(snapshot_callback_impl);
  });
  snapshot_bindings_.push_back(std::move(snapshot_callback_impl));

  if (view_state) {
    // Snapshot the child.
    view_state->top_node().Snapshot(std::move(snapshot_callback));
  } else {
    // Snapshot the entire composition.
    session_.Enqueue(
        scenic::NewTakeSnapshotCmdHACK(0, std::move(snapshot_callback)));
  }
  SchedulePresentSession();
}

// LOOKUP

ViewState* ViewRegistry::FindView(uint32_t view_token) {
  auto it = views_by_token_.find(view_token);
  return it != views_by_token_.end() ? it->second.get() : nullptr;
}

ViewTreeState* ViewRegistry::FindViewTree(uint32_t view_tree_token_value) {
  auto it = view_trees_by_token_.find(view_tree_token_value);
  return it != view_trees_by_token_.end() ? it->second.get() : nullptr;
}

}  // namespace view_manager
