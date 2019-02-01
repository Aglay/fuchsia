// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_PKG_BISCOTTI_GUEST_BIN_LINUX_COMPONENT_H_
#define GARNET_BIN_GUEST_PKG_BISCOTTI_GUEST_BIN_LINUX_COMPONENT_H_

#include <fuchsia/io/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/component/cpp/outgoing.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/function.h>
#include <lib/svc/cpp/service_provider_bridge.h>
#include <zx/eventpair.h>

namespace biscotti {

// Represents a single linux mod with an associated ViewProvider.
class LinuxComponent : public fuchsia::sys::ComponentController,
                       public fuchsia::ui::viewsv1::ViewProvider,
                       public fuchsia::ui::app::ViewProvider {
 public:
  using TerminationCallback = fit::function<void(const LinuxComponent*)>;
  static std::unique_ptr<LinuxComponent> Create(
      TerminationCallback termination_callback, fuchsia::sys::Package package,
      fuchsia::sys::StartupInfo startup_info,
      fidl::InterfaceRequest<fuchsia::sys::ComponentController> controller,
      fuchsia::ui::app::ViewProviderPtr remote_view_provider);

  ~LinuxComponent();

 private:
  TerminationCallback termination_callback_;
  fidl::Binding<fuchsia::sys::ComponentController> application_controller_;
  fidl::InterfaceRequest<fuchsia::io::Directory> directory_request_;
  component::Outgoing outgoing_;
  std::unique_ptr<component::StartupContext> startup_context_;
  fidl::BindingSet<fuchsia::ui::app::ViewProvider> view_bindings_;
  fidl::BindingSet<fuchsia::ui::viewsv1::ViewProvider> v1_view_bindings_;
  fuchsia::ui::app::ViewProviderPtr remote_view_provider_;

  LinuxComponent(
      TerminationCallback termination_callback, fuchsia::sys::Package package,
      fuchsia::sys::StartupInfo startup_info,
      fidl::InterfaceRequest<fuchsia::sys::ComponentController> controller,
      fuchsia::ui::app::ViewProviderPtr remote_view_provider);

  // |fuchsia::sys::ComponentController|
  void Kill() override;
  void Detach() override;

  // |fuchsia::ui::viewsv1::ViewProvider|
  void CreateView(
      fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> view_owner,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> services) override;

  // |fuchsia::ui::app::ViewProvider|
  void CreateView(
      zx::eventpair view_token,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
      fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services)
      override;
};

}  // namespace biscotti

#endif  // GARNET_BIN_GUEST_PKG_BISCOTTI_GUEST_BIN_LINUX_COMPONENT_H_
