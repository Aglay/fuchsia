// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/policy_checker.h"

#include "src/sys/appmgr/allow_list.h"

namespace component {
namespace {

constexpr char kDeprecatedShellAllowList[] = "allowlist/deprecated_shell.txt";
constexpr char kDeprecatedAmbientReplaceAsExecAllowList[] =
    "allowlist/deprecated_ambient_replace_as_executable.txt";
constexpr char kComponentEventProviderAllowList[] = "allowlist/component_event_provider.txt";
constexpr char kPackageResolverAllowList[] = "allowlist/package_resolver.txt";
constexpr char kPackageCacheAllowList[] = "allowlist/package_cache.txt";
constexpr char kPkgFsVersionsAllowList[] = "allowlist/pkgfs_versions.txt";

}  // end of namespace.

PolicyChecker::PolicyChecker(fxl::UniqueFD config) : config_(std::move(config)) {}

std::optional<SecurityPolicy> PolicyChecker::Check(const SandboxMetadata& sandbox,
                                                   const FuchsiaPkgUrl& fp) {
  SecurityPolicy policy;
  const std::string pkg_path = fp.ToString();
  const std::string pkg_path_without_variant = fp.WithoutVariantAndHash();

  if (CheckComponentEventProvider(pkg_path)) {
    policy.enable_component_event_provider = true;
  }
  if (sandbox.HasFeature("deprecated-ambient-replace-as-executable")) {
    if (!CheckDeprecatedAmbientReplaceAsExecutable(pkg_path)) {
      FXL_LOG(ERROR) << "Component " << pkg_path << " is not allowed to use "
                     << "deprecated-ambient-replace-as-executable. go/fx-hermetic-sandboxes";
      return std::nullopt;
    }
    policy.enable_ambient_executable = true;
  }
  if (sandbox.HasFeature("deprecated-shell") && !CheckDeprecatedShell(pkg_path)) {
    FXL_LOG(ERROR) << "Component " << pkg_path << " is not allowed to use "
                   << "deprecated-shell. go/fx-hermetic-sandboxes";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.pkg.PackageResolver") &&
      !CheckPackageResolver(pkg_path_without_variant)) {
    FXL_LOG(ERROR) << "Component " << pkg_path_without_variant << " is not allowed to use "
                   << "fuchsia.pkg.PackageResolver. go/no-package-resolver";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.pkg.PkgCache") && !CheckPackageCache(pkg_path_without_variant)) {
    FXL_LOG(ERROR) << "Component " << pkg_path_without_variant << " is not allowed to use "
                   << "fuchsia.pkg.PkgCache. go/no-package-cache";
    return std::nullopt;
  }
  if (sandbox.HasPkgFsPath("versions") && !CheckPkgFsVersions(pkg_path_without_variant)) {
    FXL_LOG(ERROR) << "Component " << pkg_path_without_variant << " is not allowed to use "
                   << "pkgfs/versions. go/no-pkgfs-versions";
    return std::nullopt;
  }
  return policy;
}

bool PolicyChecker::CheckDeprecatedAmbientReplaceAsExecutable(std::string ns_id) {
  AllowList deprecated_exec_allowlist(config_, kDeprecatedAmbientReplaceAsExecAllowList,
                                      AllowList::kOptional);
  // We treat the absence of the allowlist as an indication that we should be
  // permissive and allow all components to use replace-as-executable.  We add
  // the allowlist in user builds to ensure we are enforcing policy.
  // TODO(fxb/47836) to remove this behavior
  if (!deprecated_exec_allowlist.WasFilePresent()) {
    return true;
  }
  // Otherwise, enforce the allowlist.
  return deprecated_exec_allowlist.IsAllowed(ns_id);
}

bool PolicyChecker::CheckComponentEventProvider(std::string ns_id) {
  AllowList component_event_provider_allowlist(config_, kComponentEventProviderAllowList,
                                               AllowList::kExpected);
  return component_event_provider_allowlist.IsAllowed(ns_id);
}

bool PolicyChecker::CheckDeprecatedShell(std::string ns_id) {
  AllowList deprecated_shell_allowlist(config_, kDeprecatedShellAllowList, AllowList::kExpected);
  return deprecated_shell_allowlist.IsAllowed(ns_id);
}

bool PolicyChecker::CheckPackageResolver(std::string ns_id) {
  AllowList package_resolver_allowlist(config_, kPackageResolverAllowList, AllowList::kExpected);
  return package_resolver_allowlist.IsAllowed(ns_id);
}

bool PolicyChecker::CheckPackageCache(std::string ns_id) {
  AllowList package_cache_allowlist(config_, kPackageCacheAllowList, AllowList::kExpected);
  return package_cache_allowlist.IsAllowed(ns_id);
}

bool PolicyChecker::CheckPkgFsVersions(std::string ns_id) {
  AllowList pkgfs_versions_allowlist(config_, kPkgFsVersionsAllowList, AllowList::kExpected);
  return pkgfs_versions_allowlist.IsAllowed(ns_id);
}

}  // namespace component
