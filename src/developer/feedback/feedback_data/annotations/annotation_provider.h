// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_ANNOTATIONS_ANNOTATION_PROVIDER_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_ANNOTATIONS_ANNOTATION_PROVIDER_H_

#include <lib/fit/promise.h>

#include "src/developer/feedback/feedback_data/annotations/aliases.h"

namespace feedback {

// AnnotationProvider defines the interface all annotation providers must expose.
//
// An annotation provider will always return a subset of the annotations it supports when
// GetAnnotations() is called based on the passed allowlist. This subset is determined implicitly if
// a provider supports only one annotation (there is no need to specify which annotations to return)
// or explicitly if the provider supports multiple annotations (it needs to be told which
// annotations to get).
class AnnotationProvider {
 public:
  virtual ~AnnotationProvider() = default;

  virtual ::fit::promise<Annotations> GetAnnotations(const AnnotationKeys& allowlist) = 0;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_ANNOTATIONS_ANNOTATION_PROVIDER_H_
