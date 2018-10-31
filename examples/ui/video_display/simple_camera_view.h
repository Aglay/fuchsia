// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_EXAMPLES_UI_VIDEO_DISPLAY_SIMPLE_CAMERA_VIEW_H_
#define GARNET_EXAMPLES_UI_VIDEO_DISPLAY_SIMPLE_CAMERA_VIEW_H_

#include <deque>
#include <list>

#include <fbl/vector.h>
#include <fuchsia/simplecamera/cpp/fidl.h>
#include <lib/fxl/macros.h>
#include <lib/ui/base_view/cpp/v1_base_view.h>
#include <lib/ui/scenic/cpp/resources.h>

namespace video_display {

class SimpleCameraView : public scenic::V1BaseView {
 public:
  SimpleCameraView(scenic::ViewContext view_context);

  ~SimpleCameraView() override;

 private:
  // | scenic::V1BaseView |
  // Called when the scene is "invalidated". Invalidation happens when surface
  // dimensions or metrics change, but not necessarily when surface contents
  // change.
  void OnSceneInvalidated(
      fuchsia::images::PresentationInfo presentation_info) override;

  scenic::ShapeNode node_;

  // Client Application:
  component::Services simple_camera_provider_;
  fuchsia::sys::ComponentControllerPtr controller_;
  fuchsia::simplecamera::SimpleCameraPtr simple_camera_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SimpleCameraView);
};

}  // namespace video_display

#endif  // GARNET_EXAMPLES_UI_VIDEO_DISPLAY_SIMPLE_CAMERA_VIEW_H_
