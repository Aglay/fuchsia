// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_EXAMPLES_SWAP_CPP_MODULE_H_
#define PERIDOT_EXAMPLES_SWAP_CPP_MODULE_H_

#include "lib/module/fidl/module.fidl.h"
#include "lib/ui/view_framework/base_view.h"
#include "peridot/lib/fidl/single_service_app.h"

namespace modular_example {

class ModuleView : public mozart::BaseView {
 public:
  explicit ModuleView(
      mozart::ViewManagerPtr view_manager,
      f1dl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
      uint32_t color);

 private:
  // |BaseView|:
  void OnPropertiesChanged(mozart::ViewPropertiesPtr old_properties) override;

  scenic_lib::ShapeNode background_node_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleView);
};

class ModuleApp : public modular::SingleServiceApp<modular::Module> {
 public:
  using CreateViewCallback = std::function<mozart::BaseView*(
      mozart::ViewManagerPtr,
      f1dl::InterfaceRequest<mozart::ViewOwner>)>;

  explicit ModuleApp(app::ApplicationContext* const application_context,
                     CreateViewCallback create);

 private:
  // |SingleServiceApp|
  void CreateView(
      f1dl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
      f1dl::InterfaceRequest<app::ServiceProvider> services) override;

  // |Module|
  void Initialize(
      f1dl::InterfaceHandle<modular::ModuleContext> moduleContext,
      f1dl::InterfaceRequest<app::ServiceProvider> outgoing_services) override;

  CreateViewCallback create_;
  std::unique_ptr<mozart::BaseView> view_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleApp);
};

}  // namespace modular_example

#endif  // PERIDOT_EXAMPLES_SWAP_CPP_MODULE_H_
