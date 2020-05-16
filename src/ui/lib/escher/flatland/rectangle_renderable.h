// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_FLATLAND_RECTANGLE_RENDERABLE_H_
#define SRC_UI_LIB_ESCHER_FLATLAND_RECTANGLE_RENDERABLE_H_

#include "src/ui/lib/escher/geometry/types.h"
#include "src/ui/lib/escher/vk/texture.h"

namespace escher {

// The source spec has 4 UV coordinates for each of the
// four corners, starting at the top-left-hand corner of
// the rectangle, going clockwise. Rotations are handled
// by shifting the UV values For example, rotation by 90
// degrees would see each uv value shifted to the right
// by 1, and the uv at index 3 would wrap around to index
// 0. Rotations by 180 and 270 degrees work similary, with
// shifts of 2 and 3 respectively, instead of 1. Flipping
// the renderable about an axis can be accomplished by
// swapping UV values. For example, a horizontal flip is
// done by swapping uvs at indices 0 and 1, and at indices
// 2 and 3. A vertical flip is accomplished by swapping uvs
// at indices 0 and 3, and 1 and 2.
using ClockwiseUVs = std::array<vec2, 4>;

// Struct representing the region of an image that a
// rectangle covers. Each of the rectangle's four
// corners are explicitly listed, with the default
// values covering the whole texture with no rotation.
// UV Coordinates are stored in a union, so they can be
// accessed individually or as an array. Any rotations
// on the rectangle can be done implicitly by changing
// the uv coordinates here. Since the rectangles are
// always axis-aligned, only rotations that are multiples
// of 90 degrees are supported.
struct RectangleSourceSpec {
  RectangleSourceSpec()
      : uv_top_left(vec2(0, 0)),
        uv_top_right(vec2(1, 0)),
        uv_bottom_right(vec2(1, 1)),
        uv_bottom_left(vec2(0, 1)) {}

  RectangleSourceSpec(const ClockwiseUVs& uvs) : uv_coordinates_clockwise(uvs) {}

  union {
    struct {
      vec2 uv_top_left;
      vec2 uv_top_right;
      vec2 uv_bottom_right;
      vec2 uv_bottom_left;
    };

    // Clockwise starting at top-left.
    ClockwiseUVs uv_coordinates_clockwise;
  };
};

// Struct representing a rectangle renderable's
// dimensions on a screen. The origin represents
// the top-left-hand corner and the extent is the
// width and height. Values are given in pixels.
struct RectangleDestinationSpec {
  vec2 origin = vec2(0, 0);
  vec2 extent = vec2(1, 1);
};

// Struct representing a complete Rectangle Renderable.
// It contains both source and destination specs, a
// texture, a multiply color, and bool for transparency.
struct RectangleRenderable {
  RectangleSourceSpec source;
  RectangleDestinationSpec dest;

  // Renderer never holds onto this pointer.
  const Texture* texture = nullptr;
  vec4 color = vec4(1, 1, 1, 1);

  // If this bool is false, the renderable will render
  // as if it is opaque, even if its color or texture
  // has an alpha value less than 1.
  bool is_transparent = false;

  // Ensures that a RectangleRenderable has valid data that can be used
  // for rendering. This means making sure it has a valid texture, and
  // the the range values for its uv coordinates, extent and multiply
  // color are all within expected ranges.
  static bool IsValid(const RectangleRenderable& renderable);

  // Creates a new fully populated rectangle renderable. Rotations must
  // be in multiples of 90 degrees and a renderable without a valid texture
  // will DCHECK if passed into the rectangle compositor.
  // In local space, the rectangle renderable's top-left corner is at the origin,
  // which means that is the point of rotation.
  static const RectangleRenderable Create(const glm::mat3& matrix, const ClockwiseUVs& uvs,
                                          const Texture* texture, const glm::vec4& color,
                                          bool is_transparent);
};

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_FLATLAND_RECTANGLE_RENDERABLE_H_
