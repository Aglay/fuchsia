// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/fidl/view_host.h"

#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>

namespace modular {

struct ViewHost::ViewData {
  explicit ViewData(scenic::Session* session) : host_node(session) {}

  scenic::EntityNode host_node;
};

ViewHost::ViewHost(scenic::ViewContext view_context)
    : V1BaseView(std::move(view_context), "ViewHost"),
      container_node_(session()) {
  parent_node().AddChild(container_node_);
}

ViewHost::~ViewHost() = default;

void ViewHost::ConnectView(zx::eventpair view_holder_token) {
  const uint32_t child_key = next_child_key_++;

  auto view_data = std::make_unique<ViewData>(session());

  zx::eventpair host_import_token;
  view_data->host_node.ExportAsRequest(&host_import_token);
  container_node_.AddChild(view_data->host_node);
  views_.emplace(child_key, std::move(view_data));

  GetViewContainer()->AddChild2(child_key, std::move(view_holder_token),
                                std::move(host_import_token));
  UpdateScene();
}

void ViewHost::ConnectView(
    fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> view_owner) {
  ConnectView(zx::eventpair(view_owner.TakeChannel().release()));
}

void ViewHost::OnPropertiesChanged(
    fuchsia::ui::viewsv1::ViewProperties /*old_properties*/) {
  UpdateScene();
}

void ViewHost::OnChildUnavailable(uint32_t child_key) {
  FXL_LOG(ERROR) << "View died unexpectedly: child_key=" << child_key;

  auto it = views_.find(child_key);
  FXL_DCHECK(it != views_.end());

  it->second->host_node.Detach();
  views_.erase(it);

  GetViewContainer()->RemoveChild2(child_key, zx::eventpair());
  UpdateScene();
}

void ViewHost::UpdateScene() {
  if (!properties().view_layout || views_.empty()) {
    return;
  }

  // Layout all children in a row.
  uint32_t index = 0;
  uint32_t space = logical_size().width;
  uint32_t base = space / views_.size();
  uint32_t excess = space % views_.size();
  uint32_t offset = 0;
  for (auto it = views_.begin(); it != views_.end(); ++it, ++index) {
    ViewData* view_data = it->second.get();

    // Distribute any excess width among the leading children.
    uint32_t extent = base;
    if (excess) {
      extent++;
      excess--;
    }

    fuchsia::math::RectF layout_bounds;
    layout_bounds.x = offset;
    layout_bounds.y = 0;
    layout_bounds.width = extent;
    layout_bounds.height = logical_size().height;
    offset += extent;

    auto view_properties = fuchsia::ui::viewsv1::ViewProperties::New();
    view_properties->view_layout = fuchsia::ui::viewsv1::ViewLayout::New();
    view_properties->view_layout->size = fuchsia::math::SizeF();
    view_properties->view_layout->size.width = layout_bounds.width;
    view_properties->view_layout->size.height = layout_bounds.height;
    view_properties->view_layout->inset = fuchsia::math::InsetF();
    GetViewContainer()->SetChildProperties(it->first,
                                           std::move(view_properties));

    view_data->host_node.SetTranslation(layout_bounds.x, layout_bounds.y, 0u);
  }

  InvalidateScene();
}

}  // namespace modular
