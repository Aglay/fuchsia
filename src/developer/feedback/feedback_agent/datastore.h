// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_DATASTORE_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_DATASTORE_H_

#include <lib/async/dispatcher.h>
#include <lib/fit/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include "src/developer/feedback/feedback_agent/annotations/aliases.h"
#include "src/developer/feedback/feedback_agent/attachments/aliases.h"
#include "src/developer/feedback/utils/cobalt.h"

namespace feedback {

// Holds data useful to attach in feedback reports (crash, user feedback or bug reports).
//
// Data can be annotations or attachments.
//
// Some data are:
// * static and collected at startup, e.g., build version or hardware info.
// * dynamic and collected upon data request, e.g., uptime or logs.
// * collected synchronously, e.g., build version or uptime.
// * collected asynchronously, e.g., hardware info or logs.
//
// Because of dynamic asynchronous data, the data requests can take some time and return a
// fit::promise.
class Datastore {
 public:
  Datastore(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
            Cobalt* cobalt, zx::duration timeout, const AnnotationKeys& annotation_allowlist,
            const AttachmentKeys& attachment_allowlist);

  fit::promise<Annotations> GetAnnotations();

  fit::promise<Attachments> GetAttachments();

 private:
  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  Cobalt* cobalt_;
  const zx::duration timeout_;
  const AnnotationKeys annotation_allowlist_;
  const AttachmentKeys attachment_allowlist_;

  fit::promise<Attachment> BuildAttachment(const AttachmentKey& key);
  fit::promise<AttachmentValue> BuildAttachmentValue(const AttachmentKey& key);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_DATASTORE_H_
