// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_data/annotations/static_annotations.h"

#include <optional>
#include <string>

#include "src/developer/feedback/feedback_data/annotations/board_name_provider.h"
#include "src/developer/feedback/feedback_data/annotations/types.h"
#include "src/developer/feedback/feedback_data/annotations/utils.h"
#include "src/developer/feedback/feedback_data/constants.h"
#include "src/developer/feedback/utils/errors.h"
#include "src/lib/files/file.h"
#include "src/lib/fxl/strings/trim.h"
#include "src/lib/syslog/cpp/logger.h"

namespace feedback {
namespace {

const AnnotationKeys kSupportedAnnotations = {
    kAnnotationBuildBoard,       kAnnotationBuildProduct, kAnnotationBuildLatestCommitDate,
    kAnnotationBuildVersion,     kAnnotationBuildIsDebug, kAnnotationDeviceBoardName,
    kAnnotationDeviceFeedbackId,
};

AnnotationOr ReadStringFromFilepath(const std::string& filepath) {
  std::string content;
  if (!files::ReadFileToString(filepath, &content)) {
    return AnnotationOr(Error::kFileReadFailure);
  }
  return AnnotationOr(fxl::TrimString(content, "\r\n").ToString());
}

AnnotationOr ReadAnnotationOrFromFilepath(const AnnotationKey& key,
                                                const std::string& filepath) {
  const auto value = ReadStringFromFilepath(filepath);
  return value;
}

AnnotationOr BuildAnnotationOr(const AnnotationKey& key,
                                     DeviceIdProvider* device_id_provider) {
  if (key == kAnnotationBuildBoard) {
    return ReadAnnotationOrFromFilepath(key, "/config/build-info/board");
  } else if (key == kAnnotationBuildProduct) {
    return ReadAnnotationOrFromFilepath(key, "/config/build-info/product");
  } else if (key == kAnnotationBuildLatestCommitDate) {
    return ReadAnnotationOrFromFilepath(key, "/config/build-info/latest-commit-date");
  } else if (key == kAnnotationBuildVersion) {
    return ReadAnnotationOrFromFilepath(key, "/config/build-info/version");
  } else if (key == kAnnotationBuildIsDebug) {
#ifndef NDEBUG
    return AnnotationOr("true");
#else
    return AnnotationOr("false");
#endif
  } else if (key == kAnnotationDeviceBoardName) {
    return GetBoardName();
  } else if (key == kAnnotationDeviceFeedbackId) {
    return device_id_provider->GetId();
  }

  // We should never attempt to build a non-static annotation as a static annotation.
  FX_LOGS(FATAL) << "Attempting to get non-static annotation " << key << " as a static annotation";
  return AnnotationOr(Error::kNotSet);
}

}  // namespace

Annotations GetStaticAnnotations(const AnnotationKeys& allowlist,
                                 DeviceIdProvider* device_id_provider) {
  Annotations annotations;

  for (const auto& key : RestrictAllowlist(allowlist, kSupportedAnnotations)) {
    annotations.insert({key, BuildAnnotationOr(key, device_id_provider)});
  }
  return annotations;
}

}  // namespace feedback
