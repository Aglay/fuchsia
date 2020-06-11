// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/story_runner/module_context_impl.h"

#include <fuchsia/intl/cpp/fidl.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/inspect/cpp/component.h>

#include <string>

#include "src/lib/fxl/strings/join_strings.h"
#include "src/modular/bin/sessionmgr/agent_runner/agent_runner.h"
#include "src/modular/bin/sessionmgr/storage/encode_module_path.h"
#include "src/modular/bin/sessionmgr/story_runner/story_controller_impl.h"
#include "src/modular/lib/fidl/clone.h"

namespace modular {

ModuleContextImpl::ModuleContextImpl(
    const ModuleContextInfo& info, const fuchsia::modular::ModuleData* const module_data,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> service_provider_request)
    : module_data_(module_data),
      story_controller_impl_(info.story_controller_impl),
      session_environment_(info.session_environment),
      component_context_impl_(info.component_context_info,
                              EncodeModulePath(module_data_->module_path()),
                              module_data_->module_url()) {
  info.component_context_info.agent_runner->PublishAgentServices(
      component_context_impl_.component_instance_id(), &service_provider_impl_);
  service_provider_impl_.AddService<fuchsia::modular::ComponentContext>(
      [this](fidl::InterfaceRequest<fuchsia::modular::ComponentContext> request) {
        component_context_impl_.Connect(std::move(request));
      });
  service_provider_impl_.AddService<fuchsia::modular::ModuleContext>(
      [this](fidl::InterfaceRequest<fuchsia::modular::ModuleContext> request) {
        bindings_.AddBinding(this, std::move(request));
      });

  // Forward sessionmgr service requests to the session environment's service provider.
  // See SessionmgrImpl::InitializeSessionEnvironment.
  service_provider_impl_.AddService<fuchsia::intl::PropertyProvider>(
      [this](fidl::InterfaceRequest<fuchsia::intl::PropertyProvider> request) {
        fuchsia::sys::ServiceProviderPtr service_provider;
        session_environment_->environment()->GetServices(service_provider.NewRequest());
        service_provider->ConnectToService(fuchsia::intl::PropertyProvider::Name_,
                                           request.TakeChannel());
      });
  service_provider_impl_.AddBinding(std::move(service_provider_request));
}

ModuleContextImpl::~ModuleContextImpl() {}

void ModuleContextImpl::RemoveSelfFromStory() {
  story_controller_impl_->RemoveModuleFromStory(module_data_->module_path());
}

}  // namespace modular
