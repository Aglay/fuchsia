// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_BUGREPORT_TESTS_STUB_FEEDBACK_DATA_PROVIDER_H_
#define SRC_DEVELOPER_FEEDBACK_BUGREPORT_TESTS_STUB_FEEDBACK_DATA_PROVIDER_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>

#include "src/lib/fxl/logging.h"

namespace feedback {

// Stub fuchsia.feedback.DataProvider service that returns canned responses for
// fuchsia::feedback::DataProvider::GetData().
class StubFeedbackDataProvider : public fuchsia::feedback::DataProvider {
 public:
  StubFeedbackDataProvider(fuchsia::feedback::Attachment attachment_bundle)
      : attachment_bundle_(std::move(attachment_bundle)) {}

  // Returns a request handler for binding to this stub service.
  fidl::InterfaceRequestHandler<fuchsia::feedback::DataProvider> GetHandler() {
    return bindings_.GetHandler(this);
  }

  // |fuchsia::feedback::DataProvider|
  void GetData(GetDataCallback callback) override;
  void GetScreenshot(fuchsia::feedback::ImageEncoding encoding,
                     GetScreenshotCallback callback) override {
    FXL_NOTIMPLEMENTED();
  }

 private:
  fuchsia::feedback::Attachment attachment_bundle_;

  fidl::BindingSet<fuchsia::feedback::DataProvider> bindings_;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_BUGREPORT_TESTS_STUB_FEEDBACK_DATA_PROVIDER_H_
