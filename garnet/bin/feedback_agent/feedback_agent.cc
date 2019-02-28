// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "feedback_agent.h"

#include <map>
#include <string>

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/syslog/cpp/logger.h>

namespace fuchsia {
namespace feedback {

FeedbackAgent::FeedbackAgent(::sys::StartupContext* startup_context)
    : context_(startup_context) {
  ConnectToScenic();
}

void FeedbackAgent::GetScreenshot(ImageEncoding encoding,
                                  GetScreenshotCallback callback) {
  // We add the provided callback to the vector of pending callbacks we maintain
  // and save a reference to pass to the Scenic callback.
  auto& saved_callback = get_png_screenshot_callbacks_.emplace_back(
      std::make_unique<GetScreenshotCallback>(std::move(callback)));

  // If we previously lost the connection to Scenic, we re-attempt to establish
  // it.
  if (!is_connected_to_scenic_) {
    ConnectToScenic();
  }

  scenic_->TakeScreenshot(
      [callback = saved_callback.get()](
          fuchsia::ui::scenic::ScreenshotData raw_screenshot, bool success) {
        if (!success) {
          FX_LOGS(ERROR) << "Scenic failed to take screenshot";
          (*callback)(/*screenshot=*/nullptr);
          return;
        }

        std::unique_ptr<Screenshot> screenshot = std::make_unique<Screenshot>();
        screenshot->image = std::move(raw_screenshot.data);
        screenshot->dimensions_in_px.height = raw_screenshot.info.height;
        screenshot->dimensions_in_px.width = raw_screenshot.info.width;
        // TODO(DX-997): convert the raw image to PNG before sending it back.
        (*callback)(std::move(screenshot));
      });
}

void FeedbackAgent::ConnectToScenic() {
  scenic_ = context_->svc()->Connect<fuchsia::ui::scenic::Scenic>();
  scenic_.set_error_handler([this](zx_status_t status) {
    FX_LOGS(ERROR) << "Lost connection to Scenic service";
    is_connected_to_scenic_ = false;
    this->TerminateAllGetScreenshotCallbacks();
  });
  is_connected_to_scenic_ = true;
}

void FeedbackAgent::TerminateAllGetScreenshotCallbacks() {
  for (const auto& callback : get_png_screenshot_callbacks_) {
    (*callback)(/*screenshot=*/nullptr);
  }
  get_png_screenshot_callbacks_.clear();
}

}  // namespace feedback
}  // namespace fuchsia
