// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fsl/vmo/file.h>
#include <lib/fsl/vmo/sized_vmo.h>
#include <lib/sys/cpp/service_directory.h>

#include "src/developer/feedback/bugreport/bug_reporter.h"
#include "src/developer/feedback/utils/archive.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

class BugReporterIntegrationTest : public testing::Test {
 public:
  void SetUp() override {
    environment_services_ = sys::ServiceDirectory::CreateFromNamespace();
    ASSERT_TRUE(tmp_dir_.NewTempFile(&bugreport_path_));
  }

 protected:
  std::shared_ptr<sys::ServiceDirectory> environment_services_;
  std::string bugreport_path_;

 private:
  files::ScopedTempDir tmp_dir_;
};

TEST_F(BugReporterIntegrationTest, SmokeTest) {
  ASSERT_TRUE(MakeBugReport(environment_services_, bugreport_path_.data()));

  // We simply assert that we can unpack the bugreport archive.
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromFilename(bugreport_path_, &vmo));
  fuchsia::mem::Buffer buffer = std::move(vmo).ToTransport();
  std::vector<::fuchsia::feedback::Attachment> unpacked_attachments;
  ASSERT_TRUE(::feedback::Unpack(buffer, &unpacked_attachments));
}

}  // namespace
}  // namespace feedback
