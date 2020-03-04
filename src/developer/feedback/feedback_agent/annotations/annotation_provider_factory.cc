// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/annotations/annotation_provider_factory.h"

#include <memory>
#include <vector>

#include "src/developer/feedback/feedback_agent/annotations/aliases.h"
#include "src/developer/feedback/feedback_agent/annotations/board_info_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/board_name_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/build_info_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/channel_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/feedback_id_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/product_info_provider.h"
#include "src/developer/feedback/feedback_agent/annotations/time_provider.h"
#include "src/developer/feedback/feedback_agent/constants.h"
#include "src/developer/feedback/utils/cobalt.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/lib/timekeeper/system_clock.h"

namespace feedback {
namespace {

// Generate a vector that contains all of the values of an enum class.
template <typename Enum, Enum First, Enum Last>
std::vector<Enum> AllEnumValues() {
  std::vector<Enum> values(static_cast<size_t>(Last) - static_cast<size_t>(First) + 1);
  std::generate(values.begin(), values.end(),
                [n = static_cast<size_t>(First)]() mutable { return static_cast<Enum>(n++); });
  return values;
}

// The type of annotations.
enum class AnnotationType {
  BoardName = 0,
  BuildInfo,
  Channel,
  FeedbackId,
  HardwareBoardInfo,
  HardwareProductInfo,
  Time,
};

const auto GetAnnotationTypes =
    AllEnumValues<AnnotationType, AnnotationType::BoardName, AnnotationType::Time>;

AnnotationKeys GetSupportedAnnotations(const AnnotationType type) {
  switch (type) {
    case AnnotationType::BoardName:
      return BoardNameProvider::GetSupportedAnnotations();
    case AnnotationType::BuildInfo:
      return BuildInfoProvider::GetSupportedAnnotations();
    case AnnotationType::Channel:
      return ChannelProvider::GetSupportedAnnotations();
    case AnnotationType::FeedbackId:
      return FeedbackIdProvider::GetSupportedAnnotations();
    case AnnotationType::HardwareBoardInfo:
      return BoardInfoProvider::GetSupportedAnnotations();
    case AnnotationType::HardwareProductInfo:
      return ProductInfoProvider::GetSupportedAnnotations();
    case AnnotationType::Time:
      return TimeProvider::GetSupportedAnnotations();
  }
}

AnnotationKeys AnnotationsToCollect(const AnnotationType type, const AnnotationKeys& allowlist) {
  const AnnotationKeys supported = GetSupportedAnnotations(type);

  std::vector<AnnotationKey> intersection;
  std::set_intersection(allowlist.begin(), allowlist.end(), supported.begin(), supported.end(),
                        std::back_inserter(intersection));
  return std::set(intersection.begin(), intersection.end());
}

std::unique_ptr<AnnotationProvider> GetProvider(const AnnotationType type,
                                                const AnnotationKeys& annotations,
                                                async_dispatcher_t* dispatcher,
                                                std::shared_ptr<sys::ServiceDirectory> services,
                                                const zx::duration timeout, Cobalt* cobalt) {
  switch (type) {
    case AnnotationType::BoardName:
      return std::make_unique<BoardNameProvider>();
    case AnnotationType::BuildInfo:
      return std::make_unique<BuildInfoProvider>(annotations);
    case AnnotationType::Channel:
      return std::make_unique<ChannelProvider>(dispatcher, services, timeout, std::move(cobalt));
    case AnnotationType::FeedbackId:
      return std::make_unique<FeedbackIdProvider>();
    case AnnotationType::HardwareBoardInfo:
      return std::make_unique<BoardInfoProvider>(annotations, dispatcher, services, timeout,
                                                 cobalt);
    case AnnotationType::HardwareProductInfo:
      return std::make_unique<ProductInfoProvider>(annotations, dispatcher, services, timeout,
                                                   cobalt);
    case AnnotationType::Time:
      return std::make_unique<TimeProvider>(annotations,
                                            std::make_unique<timekeeper::SystemClock>());
  }
}

AnnotationKeys AddIfAnnotationsIntersect(
    const AnnotationType type, const AnnotationKeys& allowlist, async_dispatcher_t* dispatcher,
    std::shared_ptr<sys::ServiceDirectory> services, const zx::duration timeout, Cobalt* cobalt,
    std::vector<std::unique_ptr<AnnotationProvider>>* providers) {
  auto annotations = AnnotationsToCollect(type, allowlist);
  if (!annotations.empty()) {
    providers->push_back(GetProvider(type, annotations, dispatcher, services, timeout, cobalt));
  }

  return annotations;
}

}  // namespace

std::vector<std::unique_ptr<AnnotationProvider>> GetProviders(
    const AnnotationKeys& allowlist, async_dispatcher_t* dispatcher,
    std::shared_ptr<sys::ServiceDirectory> services, const zx::duration timeout, Cobalt* cobalt) {
  static auto annotation_types = GetAnnotationTypes();

  AnnotationKeys ignored_annotations = allowlist;
  std::vector<std::unique_ptr<AnnotationProvider>> providers;
  for (const auto& type : annotation_types) {
    const auto annotations = AddIfAnnotationsIntersect(type, allowlist, dispatcher, services,
                                                       timeout, cobalt, &providers);

    for (const auto& annotation : annotations) {
      ignored_annotations.erase(annotation);
    }
  }

  for (const auto& annotation : ignored_annotations) {
    FX_LOGS(WARNING) << "Annotation " << annotation
                     << " is not supported and will not be collected";
  }

  return providers;
}

}  // namespace feedback
