// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_UTILS_ARCHIVE_H_
#define SRC_DEVELOPER_FORENSICS_UTILS_ARCHIVE_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>

#include <vector>

namespace forensics {

// Bundles a vector of attachments into a single ZIP archive with DEFLATE compression.
bool Archive(const std::vector<fuchsia::feedback::Attachment>& attachments,
             fuchsia::mem::Buffer* archive);

// Unpack a ZIP archive into a vector of attachments.
bool Unpack(const fuchsia::mem::Buffer& archive,
            std::vector<fuchsia::feedback::Attachment>* attachments);

}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_UTILS_ARCHIVE_H_
