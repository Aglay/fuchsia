// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_ANNOTATIONS_CHANNEL_PROVIDER_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_ANNOTATIONS_CHANNEL_PROVIDER_H_

#include <lib/async/dispatcher.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include <memory>

#include "src/developer/feedback/feedback_data/annotations/annotation_provider.h"
#include "src/developer/feedback/feedback_data/annotations/types.h"
#include "src/developer/feedback/utils/cobalt/logger.h"
#include "src/lib/fxl/macros.h"

namespace feedback {

class ChannelProvider : public AnnotationProvider {
 public:
  // fuchsia.update.channel.Provider is expected to be in |services|.
  ChannelProvider(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
                  zx::duration timeout, cobalt::Logger* cobalt);

  ::fit::promise<Annotations> GetAnnotations(const AnnotationKeys& allowlist) override;

 private:
  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  const zx::duration timeout_;
  cobalt::Logger* cobalt_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ChannelProvider);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_DATA_ANNOTATIONS_CHANNEL_PROVIDER_H_
