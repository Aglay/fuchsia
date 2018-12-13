// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/sysmgr/delegating_loader.h"

#include <lib/async/default.h>
#include "lib/fidl/cpp/clone.h"
#include "lib/svc/cpp/services.h"

namespace sysmgr {
namespace {

std::string GetScheme(const std::string& url) {
  size_t pos = url.find(':');
  if (pos == std::string::npos)
    return std::string();
  return url.substr(0, pos);
}

}  // namespace

// static
std::unique_ptr<DelegatingLoader> DelegatingLoader::MakeWithParentFallback(
    Config::ServiceMap delegates, fuchsia::sys::Launcher* delegate_launcher,
    fuchsia::sys::LoaderPtr fallback) {
  return std::unique_ptr<DelegatingLoader>(new DelegatingLoader(
      std::move(delegates), delegate_launcher, std::move(fallback),
      std::unordered_set<std::string>{}, nullptr));
}

// static
std::unique_ptr<DelegatingLoader>
DelegatingLoader::MakeWithPackageUpdatingFallback(
    Config::ServiceMap delegates, fuchsia::sys::Launcher* delegate_launcher,
    std::unordered_set<std::string> update_dependency_urls,
    fuchsia::pkg::PackageResolverPtr resolver) {
  return std::unique_ptr<DelegatingLoader>(new DelegatingLoader(
      std::move(delegates), delegate_launcher, nullptr,
      std::move(update_dependency_urls), std::move(resolver)));
}

DelegatingLoader::DelegatingLoader(
    Config::ServiceMap delegates, fuchsia::sys::Launcher* delegate_launcher,
    fuchsia::sys::LoaderPtr fallback,
    std::unordered_set<std::string> update_dependency_urls,
    fuchsia::pkg::PackageResolverPtr resolver)
    : delegate_launcher_(delegate_launcher),
      parent_fallback_(std::move(fallback)) {
  for (auto& pair : delegates) {
    auto& record = delegate_instances_[pair.second->url];
    record.launch_info = std::move(pair.second);
    delegates_by_scheme_[pair.first] = &record;
  }
  if (resolver) {
    package_updating_fallback_ = std::make_unique<PackageUpdatingLoader>(
        std::move(update_dependency_urls), std::move(resolver),
        async_get_default_dispatcher());
  }
}

DelegatingLoader::~DelegatingLoader() = default;

void DelegatingLoader::LoadUrl(fidl::StringPtr url, LoadUrlCallback callback) {
  std::string scheme = GetScheme(url);
  if (!scheme.empty()) {
    auto it = delegates_by_scheme_.find(scheme);
    if (it != delegates_by_scheme_.end()) {
      auto* record = it->second;
      if (!record->loader) {
        StartDelegate(record);
      }
      record->loader->LoadUrl(url, std::move(callback));
      return;
    }
  }

  if (package_updating_fallback_) {
    package_updating_fallback_->LoadUrl(url, callback);
  } else {
    parent_fallback_->LoadUrl(url, callback);
  }
}

void DelegatingLoader::StartDelegate(LoaderRecord* record) {
  component::Services services;
  fuchsia::sys::LaunchInfo dup_launch_info;
  dup_launch_info.url = record->launch_info->url;
  fidl::Clone(record->launch_info->arguments, &dup_launch_info.arguments);
  dup_launch_info.directory_request = services.NewRequest();
  delegate_launcher_->CreateComponent(std::move(dup_launch_info),
                                      record->controller.NewRequest());

  record->loader = services.ConnectToService<fuchsia::sys::Loader>();
  record->loader.set_error_handler([this, record](zx_status_t status) {
    // proactively kill the loader app entirely if its Loader died on
    // us
    record->controller.Unbind();
  });
}

}  // namespace sysmgr
