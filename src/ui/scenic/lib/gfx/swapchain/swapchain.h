// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_GFX_SWAPCHAIN_SWAPCHAIN_H_
#define SRC_UI_SCENIC_LIB_GFX_SWAPCHAIN_SWAPCHAIN_H_

#include <lib/fit/function.h>
#include <lib/zx/time.h>

#include <array>

#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/ui/scenic/lib/gfx/displays/color_transform.h"
#include "src/ui/scenic/lib/gfx/engine/hardware_layer_assignment.h"

namespace escher {
class Image;
class Semaphore;
using ImagePtr = fxl::RefPtr<Image>;
using SemaphorePtr = fxl::RefPtr<Semaphore>;
};  // namespace escher

namespace scenic_impl {
namespace gfx {

class FrameTimings;
using FrameTimingsPtr = fxl::RefPtr<FrameTimings>;

// Swapchain is an interface used used to render into an escher::Image and
// present the result (to a physical display or elsewhere).
class Swapchain {
 public:
  // Callback used to draw a frame. Arguments are:
  // - the framebuffer to render into.
  // - the semaphore to wait upon before rendering into the framebuffer
  // - the semaphore to signal when rendering is complete.
  using DrawCallback =
      fit::function<void(zx::time target_presentation_time, const escher::ImagePtr&,
                         const HardwareLayerAssignment::Item&, const escher::SemaphorePtr&,
                         const escher::SemaphorePtr&)>;

  // Returns false if the frame could not be drawn.  Otherwise,
  //   1. Invokes |draw_callback| to draw the frame.
  //   2. Eventually invokes FrameTimings::OnFrameFinishedRendering() and
  //      FrameTimings::OnFramePresented() on |frame_timings|.
  virtual bool DrawAndPresentFrame(const FrameTimingsPtr& frame, size_t swapchain_index,
                                   const HardwareLayerAssignment& hla,
                                   DrawCallback draw_callback) = 0;

  // If a swapchain subclass implements this interface has a display,
  // this function passes along color correction information to the
  // display. The three parameters modify the output display pixels
  // using the following formula:
  // (coefficients * (pixel + preoffsets)) + postoffsets.
  virtual void SetDisplayColorConversion(const ColorTransform& transform) = 0;

  virtual ~Swapchain() = default;
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // SRC_UI_SCENIC_LIB_GFX_SWAPCHAIN_SWAPCHAIN_H_
