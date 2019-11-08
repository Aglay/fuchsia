// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE
// file.

#include "src/developer/feedback/feedback_agent/annotations/annotation_provider_factory.h"

#include "src/developer/feedback/feedback_agent/annotations/board_name_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/build_info_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/channel_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/uptime_provider.h"
#include "src/developer/feedback/feedback_agent/constants.h"

namespace feedback {
namespace {

// The type of annotations.
enum class AnnotationType {
  BoardName = 0,
  BuildInfo,
  Channel,
  Uptime,
};

std::set<std::string> GetSupportedAnnotations(const AnnotationType type) {
  switch (type) {
    case AnnotationType::BoardName:
      return BoardNameProvider::GetSupportedAnnotations();
    case AnnotationType::BuildInfo:
      return BuildInfoProvider::GetSupportedAnnotations();
    case AnnotationType::Channel:
      return ChannelProvider::GetSupportedAnnotations();
    case AnnotationType::Uptime:
      return UptimeProvider::GetSupportedAnnotations();
  }
}

std::set<std::string> AnnotationsToCollect(const AnnotationType type,
                                           const std::set<std::string>& allowlist) {
  const std::set<std::string> supported = GetSupportedAnnotations(type);

  std::vector<std::string> intersection;
  std::set_intersection(allowlist.begin(), allowlist.end(), supported.begin(), supported.end(),
                        std::back_inserter(intersection));
  return std::set(intersection.begin(), intersection.end());
}

std::unique_ptr<AnnotationProvider> GetProvider(const AnnotationType type,
                                                const std::set<std::string>& annotations,
                                                async_dispatcher_t* dispatcher,
                                                std::shared_ptr<sys::ServiceDirectory> services,
                                                const zx::duration timeout) {
  switch (type) {
    case AnnotationType::BoardName:
      return std::make_unique<BoardNameProvider>();
    case AnnotationType::BuildInfo:
      return std::make_unique<BuildInfoProvider>(annotations);
    case AnnotationType::Channel:
      return std::make_unique<ChannelProvider>(dispatcher, services, timeout);
    case AnnotationType::Uptime:
      return std::make_unique<UptimeProvider>();
  }
}

void AddIfAnnotationsIntersect(const AnnotationType type, const std::set<std::string>& allowlist,
                               async_dispatcher_t* dispatcher,
                               std::shared_ptr<sys::ServiceDirectory> services,
                               const zx::duration timeout,
                               std::vector<std::unique_ptr<AnnotationProvider>>* providers) {
  auto annotations = AnnotationsToCollect(type, allowlist);
  if (annotations.empty()) {
    return;
  }

  providers->push_back(GetProvider(type, annotations, dispatcher, services, timeout));
}

}  // namespace

std::vector<std::unique_ptr<AnnotationProvider>> GetProviders(
    const std::set<std::string>& allowlist, async_dispatcher_t* dispatcher,
    std::shared_ptr<sys::ServiceDirectory> services, const zx::duration timeout) {
  std::vector<std::unique_ptr<AnnotationProvider>> providers;

  AddIfAnnotationsIntersect(AnnotationType::BoardName, allowlist, dispatcher, services, timeout,
                            &providers);
  AddIfAnnotationsIntersect(AnnotationType::BuildInfo, allowlist, dispatcher, services, timeout,
                            &providers);
  AddIfAnnotationsIntersect(AnnotationType::Channel, allowlist, dispatcher, services, timeout,
                            &providers);
  AddIfAnnotationsIntersect(AnnotationType::Uptime, allowlist, dispatcher, services, timeout,
                            &providers);

  return providers;
}

}  // namespace feedback
