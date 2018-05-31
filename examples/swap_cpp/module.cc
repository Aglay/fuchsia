// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/examples/swap_cpp/module.h"

#include <utility>

#include "lib/fsl/tasks/message_loop.h"

namespace modular_example {

ModuleView::ModuleView(
    fuchsia::ui::views_v1::ViewManagerPtr view_manager,
    fidl::InterfaceRequest<fuchsia::ui::views_v1_token::ViewOwner> view_owner_request,
    uint32_t color)
    : BaseView(std::move(view_manager),
               std::move(view_owner_request),
               "ModuleView"),
      background_node_(session()) {
  scenic_lib::Material background_material(session());
  background_material.SetColor((color >> 16) & 0xff, (color >> 8) & 0xff,
                               color & 0xff, (color >> 24) & 0xff);
  background_node_.SetMaterial(background_material);
  parent_node().AddChild(background_node_);
}

void ModuleView::OnPropertiesChanged(fuchsia::ui::views_v1::ViewProperties) {
  scenic_lib::Rectangle background_shape(session(), logical_size().width,
                                         logical_size().height);
  background_node_.SetShape(background_shape);
  background_node_.SetTranslation(logical_size().width * .5f,
                                  logical_size().height * .5f, 0.f);
  InvalidateScene();
}

ModuleApp::ModuleApp(component::ApplicationContext* const application_context,
                     CreateViewCallback create)
    : ViewApp(application_context), create_(std::move(create)) {}

void ModuleApp::CreateView(
    fidl::InterfaceRequest<fuchsia::ui::views_v1_token::ViewOwner> view_owner_request,
    fidl::InterfaceRequest<component::ServiceProvider> /*services*/) {
  view_.reset(
      create_(application_context()
                  ->ConnectToEnvironmentService<fuchsia::ui::views_v1::ViewManager>(),
              std::move(view_owner_request)));
}

}  // namespace modular_example
