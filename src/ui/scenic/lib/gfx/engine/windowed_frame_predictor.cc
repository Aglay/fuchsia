// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/engine/windowed_frame_predictor.h"

#include <src/lib/fxl/logging.h>
#include <trace/event.h>

namespace scenic_impl {
namespace gfx {

WindowedFramePredictor::WindowedFramePredictor(zx::duration initial_render_duration_prediction,
                                               zx::duration initial_update_duration_prediction)
    : render_duration_predictor_(kRenderPredictionWindowSize, initial_render_duration_prediction),
      update_duration_predictor_(kUpdatePredictionWindowSize, initial_update_duration_prediction) {}

WindowedFramePredictor::~WindowedFramePredictor() {}

void WindowedFramePredictor::ReportRenderDuration(zx::duration time_to_render) {
  FXL_DCHECK(time_to_render >= zx::duration(0));
  render_duration_predictor_.InsertNewMeasurement(time_to_render);
}

void WindowedFramePredictor::ReportUpdateDuration(zx::duration time_to_update) {
  FXL_DCHECK(time_to_update >= zx::duration(0));
  update_duration_predictor_.InsertNewMeasurement(time_to_update);
}

zx::duration WindowedFramePredictor::PredictTotalRequiredDuration() const {
  const zx::duration predicted_time_to_update = update_duration_predictor_.GetPrediction();
  const zx::duration predicted_time_to_render = render_duration_predictor_.GetPrediction();

  const zx::duration predicted_frame_duration = std::min(
      kMaxFrameTime, predicted_time_to_update + predicted_time_to_render + kHardcodedMargin);

  // Pretty print the times in milliseconds.
  TRACE_INSTANT("gfx", "WindowedFramePredictor::GetPrediction", TRACE_SCOPE_PROCESS,
                "Predicted frame duration(ms)",
                static_cast<double>(predicted_frame_duration.to_usecs()) / 1000, "Render time(ms)",
                static_cast<double>(predicted_time_to_render.to_usecs()) / 1000, "Update time(ms)",
                static_cast<double>(predicted_time_to_update.to_usecs()) / 1000);

  return predicted_frame_duration;
}

PredictedTimes WindowedFramePredictor::GetPrediction(PredictionRequest request) {
#if SCENIC_IGNORE_VSYNC
  // Predict that the frame should be rendered immediately.
  return {.presentation_time = request.now, .latch_point_time = request.now};
#endif
  return ComputePredictionFromDuration(request, PredictTotalRequiredDuration());
}

}  // namespace gfx
}  // namespace scenic_impl
