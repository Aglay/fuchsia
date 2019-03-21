// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_A11Y_A11Y_MANAGER_MANAGER_IMPL_H_
#define GARNET_BIN_A11Y_A11Y_MANAGER_MANAGER_IMPL_H_

#include <unordered_map>

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <lib/sys/cpp/component_context.h>

#include "garnet/bin/a11y/a11y_manager/semantic_tree.h"
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"

namespace a11y_manager {

// Accessibility manager interface implementation.
// See manager.fidl for documentation.
class ManagerImpl : public fuchsia::accessibility::Manager {
 public:
  ManagerImpl() = default;
  ~ManagerImpl() = default;

  void AddBinding(
      fidl::InterfaceRequest<fuchsia::accessibility::Manager> request);

 private:
  // |fuchsia::accessibility::Manager|
  void GetHitAccessibilityNode(
      fuchsia::ui::viewsv1::ViewTreeToken token,
      fuchsia::ui::input::PointerEvent input,
      GetHitAccessibilityNodeCallback callback) override;
  void SetAccessibilityFocus(int32_t view_id, int32_t node_id) override;
  void PerformAccessibilityAction(
      fuchsia::accessibility::Action action) override;

  void BroadcastOnNodeAccessibilityAction(
      int32_t id, fuchsia::accessibility::Node node,
      fuchsia::accessibility::Action action);

  fidl::BindingSet<fuchsia::accessibility::Manager> bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ManagerImpl);
};

}  // namespace a11y_manager

#endif  // GARNET_BIN_A11Y_A11Y_MANAGER_MANAGER_IMPL_H_
