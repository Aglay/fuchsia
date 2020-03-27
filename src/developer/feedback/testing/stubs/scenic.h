// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_SCENIC_H_
#define SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_SCENIC_H_

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>

#include <cstdint>
#include <vector>

#include "src/lib/fxl/logging.h"

namespace feedback {
namespace stubs {

// Returns an empty screenshot, still needed when Scenic::TakeScreenshot() returns false as the FIDL
// ScreenshotData field is not marked optional in fuchsia.ui.scenic.Scenic.TakeScreenshot.
fuchsia::ui::scenic::ScreenshotData CreateEmptyScreenshot();

// Returns an 8-bit BGRA image of a |image_dim_in_px| x |image_dim_in_px| checkerboard, where each
// white/black region is a 10x10 pixel square.
fuchsia::ui::scenic::ScreenshotData CreateCheckerboardScreenshot(const size_t image_dim_in_px);

// Returns an empty screenshot with a pixel format different from BGRA-8.
fuchsia::ui::scenic::ScreenshotData CreateNonBGRA8Screenshot();

// Represents arguments for Scenic::TakeScreenshot().
struct TakeScreenshotResponse {
  fuchsia::ui::scenic::ScreenshotData screenshot;
  bool success;

  TakeScreenshotResponse(fuchsia::ui::scenic::ScreenshotData data, bool success)
      : screenshot(std::move(data)), success(success){};
};

class Scenic : public fuchsia::ui::scenic::testing::Scenic_TestBase {
 public:
  fidl::InterfaceRequestHandler<fuchsia::ui::scenic::Scenic> GetHandler() {
    return [this](fidl::InterfaceRequest<fuchsia::ui::scenic::Scenic> request) {
      total_num_bindings_++;
      bindings_.AddBinding(this, std::move(request));
    };
  }

  void CloseAllConnections() { bindings_.CloseAll(); }

  // |fuchsia::ui::scenic::Scenic|.
  void TakeScreenshot(TakeScreenshotCallback callback) override;

  // |fuchsia::ui::scenic::testing::Scenic_TestBase|
  void NotImplemented_(const std::string& name) override {
    FXL_NOTIMPLEMENTED() << name << " is not implemented";
  }

  uint64_t total_num_bindings() { return total_num_bindings_; }
  size_t current_num_bindings() { return bindings_.size(); }

  //  injection and verification methods.
  void set_take_screenshot_responses(std::vector<TakeScreenshotResponse> responses) {
    take_screenshot_responses_ = std::move(responses);
  }
  const std::vector<TakeScreenshotResponse>& take_screenshot_responses() const {
    return take_screenshot_responses_;
  }

 private:
  fidl::BindingSet<fuchsia::ui::scenic::Scenic> bindings_;
  uint64_t total_num_bindings_ = 0;
  std::vector<TakeScreenshotResponse> take_screenshot_responses_;
};

class ScenicAlwaysReturnsFalse : public Scenic {
 public:
  // |fuchsia::ui::scenic::Scenic|.
  void TakeScreenshot(TakeScreenshotCallback callback) override;
};

class ScenicClosesConnection : public Scenic {
 public:
  // |fuchsia::ui::scenic::Scenic|.
  void TakeScreenshot(TakeScreenshotCallback callback) override;
};

class ScenicNeverReturns : public Scenic {
 public:
  // |fuchsia::ui::scenic::Scenic|.
  void TakeScreenshot(TakeScreenshotCallback callback) override;
};

}  // namespace stubs
}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_SCENIC_H_
