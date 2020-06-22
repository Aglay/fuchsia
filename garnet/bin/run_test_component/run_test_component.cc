// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/run_test_component/run_test_component.h"

#include <fuchsia/sys/index/cpp/fidl.h>
#include <glob.h>
#include <lib/fit/defer.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/syslog/logger.h>
#include <limits.h>

#include <regex>
#include <sstream>
#include <string>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/fxl/strings/substitute.h"
#include "src/lib/pkg_url/fuchsia_pkg_url.h"

namespace run {

using fuchsia::sys::index::ComponentIndex_FuzzySearch_Result;
using fuchsia::sys::index::ComponentIndexSyncPtr;

static constexpr char kComponentIndexerUrl[] =
    "fuchsia-pkg://fuchsia.com/component_index#meta/component_index.cmx";

static constexpr char kLabelArgPrefix[] = "--realm-label=";
static constexpr char kTimeoutArgPrefix[] = "--timeout=";
static constexpr char kSeverityArgPrefix[] = "--min-severity-logs=";
static constexpr char kRestrictLogsArgPrefix[] = "--restrict-logs";

bool to_bool(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(), ::tolower);
  std::istringstream is(str);
  bool b;
  is >> std::boolalpha >> b;
  return b;
}

ParseArgsResult ParseArgs(const std::shared_ptr<sys::ServiceDirectory>& services, int argc,
                          const char** argv) {
  ParseArgsResult result;
  result.error = false;
  result.timeout = -1;
  int url_or_matcher_argi = 1;

  std::string url;
  while (true) {
    if (argc < url_or_matcher_argi + 1) {
      result.error = true;
      result.error_msg = "Missing test URL, or matcher argument";
      return result;
    }

    std::string argument = argv[url_or_matcher_argi];
    const size_t kLabelArgPrefixLength = strlen(kLabelArgPrefix);
    const size_t kTimeoutArgPrefixLength = strlen(kTimeoutArgPrefix);
    const size_t kSeverityArgPrefixLength = strlen(kSeverityArgPrefix);
    const size_t kRestrictLogsArgPrefixLength = strlen(kRestrictLogsArgPrefix);
    if (argument.substr(0, kLabelArgPrefixLength) == kLabelArgPrefix) {
      result.realm_label = argument.substr(kLabelArgPrefixLength);
      url_or_matcher_argi++;
      continue;
    }
    if (argument.substr(0, kSeverityArgPrefixLength) == kSeverityArgPrefix) {
      std::string level = argument.substr(kSeverityArgPrefixLength);
      if (level == "TRACE") {
        result.min_log_severity = FX_LOG_TRACE;
      } else if (level == "DEBUG") {
        result.min_log_severity = FX_LOG_DEBUG;
      } else if (level == "INFO") {
        result.min_log_severity = FX_LOG_INFO;
      } else if (level == "WARN") {
        result.min_log_severity = FX_LOG_WARNING;
      } else if (level == "ERROR") {
        result.min_log_severity = FX_LOG_ERROR;
      } else if (level == "FATAL") {
        result.min_log_severity = FX_LOG_FATAL;
      } else {
        result.error = true;
        result.error_msg = fxl::StringPrintf("Invalid severity %s", level.c_str());
        return result;
      }
      url_or_matcher_argi++;
      continue;
    }

    if (argument.substr(0, kRestrictLogsArgPrefixLength) == kRestrictLogsArgPrefix) {
      std::string arg = argument.substr(kRestrictLogsArgPrefixLength);
      result.restrict_logs = false;
      if (arg.length() == 0) {
        result.restrict_logs = true;
      } else {
        if (arg[0] != '=') {
          result.error = true;
          result.error_msg = fxl::StringPrintf("\"%s\" is not a valid argument.", argument.c_str());
          return result;
        }
        arg = arg.substr(1);
        result.restrict_logs = to_bool(std::move(arg));
      }
      url_or_matcher_argi++;
      continue;
    }

    if (argument.substr(0, kTimeoutArgPrefixLength) == kTimeoutArgPrefix) {
      std::string arg = argument.substr(kTimeoutArgPrefixLength);
      std::istringstream(arg) >> result.timeout;
      if (result.timeout <= 0 || result.timeout == INT_MIN || result.timeout == INT_MAX) {
        result.error = true;
        result.error_msg = fxl::StringPrintf("\"%s\" is not a valid timeout.", arg.c_str());
        return result;
      }
      url_or_matcher_argi++;
      continue;
    }

    url = argument;
    break;
  }

  if (!component::FuchsiaPkgUrl::IsFuchsiaPkgScheme(url)) {
    fuchsia::sys::LaunchInfo index_launch_info;
    index_launch_info.url = kComponentIndexerUrl;
    auto index_provider =
        sys::ServiceDirectory::CreateWithRequest(&index_launch_info.directory_request);

    // Connect to the Launcher service through our static environment.
    fuchsia::sys::LauncherSyncPtr launcher;
    services->Connect(launcher.NewRequest());
    fuchsia::sys::ComponentControllerPtr component_index_controller;
    launcher->CreateComponent(std::move(index_launch_info),
                              component_index_controller.NewRequest());

    ComponentIndexSyncPtr index;
    index_provider->Connect(index.NewRequest());

    std::string test_name = url;
    ComponentIndex_FuzzySearch_Result fuzzy_search_result;
    zx_status_t status = index->FuzzySearch(test_name, &fuzzy_search_result);
    if (status != ZX_OK) {
      result.error = true;
      result.error_msg = fxl::StringPrintf(
          "\"%s\" is not a valid URL. Attempted to match to a URL with "
          "fuchsia.sys.index.FuzzySearch, but the service is not available.",
          test_name.c_str());
      return result;
    }

    if (fuzzy_search_result.is_err()) {
      result.error = true;
      result.error_msg = fxl::StringPrintf(
          "\"%s\" contains unsupported characters for fuzzy "
          "matching. Valid characters are [A-Z a-z 0-9 / _ - .].\n",
          test_name.c_str());
      return result;
    } else {
      std::vector<std::string> uris = fuzzy_search_result.response().uris;
      if (uris.size() == 0) {
        result.error = true;
        result.error_msg =
            fxl::StringPrintf("\"%s\" did not match any components.\n", test_name.c_str());
        return result;
      } else {
        for (auto& uri : uris) {
          result.matching_urls.push_back(uri);
        }
        if (uris.size() > 1) {
          return result;
        }
        url = uris[0];
      }
    }
  }

  result.launch_info.url = url;
  result.launch_info.arguments.emplace();
  for (int i = url_or_matcher_argi + 1; i < argc; i++) {
    result.launch_info.arguments->push_back(argv[i]);
  }
  return result;
}

std::string GetSimplifiedUrl(const std::string& url) {
  component::FuchsiaPkgUrl furl;
  furl.Parse(url);
  return fxl::Substitute("fuchsia-pkg://$0/$1#$2", furl.host_name(), furl.package_name(),
                         furl.resource_path());
}

}  // namespace run
