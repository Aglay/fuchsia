// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_AGENT_FEEDBACK_AGENT_H_
#define SRC_DEVELOPER_FEEDBACK_AGENT_FEEDBACK_AGENT_H_

#include <memory>
#include <string>
#include <vector>

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/sys/cpp/component_context.h>

namespace fuchsia {
namespace feedback {

// Provides data useful to attach in feedback reports (crash or user feedback).
class FeedbackAgent : public DataProvider {
 public:
  FeedbackAgent(::sys::ComponentContext* startup_context);

  // Returns all the feedback data except the screenshot, which is provided
  // separately.
  void GetData(GetDataCallback callback) override;

  // Returns an image of the current view encoded in the provided |encoding|.
  void GetScreenshot(ImageEncoding encoding,
                     GetScreenshotCallback callback) override;

 private:
  // Connects to Scenic and sets up the error handler in case we lose the
  // connection.
  void ConnectToScenic();

  // Signals to all the pending GetScreenshot callbacks that an error occurred,
  // most likely the loss of the connection with Scenic.
  void TerminateAllGetScreenshotCallbacks();

  ::sys::ComponentContext* context_;

  fuchsia::ui::scenic::ScenicPtr scenic_;
  bool is_connected_to_scenic_;
  // We keep track of the pending GetScreenshot callbacks so we can terminate
  // all of them when we lose the connection with Scenic.
  std::vector<std::unique_ptr<GetScreenshotCallback>>
      get_png_screenshot_callbacks_;
};

}  // namespace feedback
}  // namespace fuchsia

#endif  // SRC_DEVELOPER_FEEDBACK_AGENT_FEEDBACK_AGENT_H_
