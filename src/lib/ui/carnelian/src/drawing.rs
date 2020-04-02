// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Functions for drawing in Carnelian
//! Carnelian uses the Render abstraction over Mold and Spinel
//! to put pixels on screen. The items in this module are higher-
//! level drawing primitives.

use crate::{
    color::Color,
    geometry::{Coord, Corners, Point, Rect},
    render::{Context as RenderContext, Path, Raster},
};
use anyhow::Error;
use euclid::{Box2D, Vector2D};
use rusttype::{Font, FontCollection, GlyphId, Scale, Segment};
use std::collections::BTreeMap;
use textwrap::wrap_iter;

/// Create a render path for the specified rectangle.
pub fn path_for_rectangle(bounds: &Rect, render_context: &mut RenderContext) -> Path {
    let mut path_builder = render_context.path_builder().expect("path_builder");
    path_builder
        .move_to(bounds.origin)
        .line_to(bounds.top_right())
        .line_to(bounds.bottom_right())
        .line_to(bounds.bottom_left())
        .line_to(bounds.origin);
    path_builder.build()
}

/// Create a render path for the specified rounded rectangle.
pub fn path_for_rounded_rectangle(
    bounds: &Rect,
    corner_radius: Coord,
    render_context: &mut RenderContext,
) -> Path {
    let kappa = 4.0 / 3.0 * (std::f32::consts::PI / 8.0).tan();
    let control_dist = kappa * corner_radius;

    let top_left_arc_start = bounds.origin + Vector2D::new(0.0, corner_radius);
    let top_left_arc_end = bounds.origin + Vector2D::new(corner_radius, 0.0);
    let top_left_curve_center = bounds.origin + Vector2D::new(corner_radius, corner_radius);
    let top_left_p1 = top_left_curve_center + Vector2D::new(-corner_radius, -control_dist);
    let top_left_p2 = top_left_curve_center + Vector2D::new(-control_dist, -corner_radius);

    let top_right = bounds.top_right();
    let top_right_arc_start = top_right + Vector2D::new(-corner_radius, 0.0);
    let top_right_arc_end = top_right + Vector2D::new(0.0, corner_radius);
    let top_right_curve_center = top_right + Vector2D::new(-corner_radius, corner_radius);
    let top_right_p1 = top_right_curve_center + Vector2D::new(control_dist, -corner_radius);
    let top_right_p2 = top_right_curve_center + Vector2D::new(corner_radius, -control_dist);

    let bottom_right = bounds.bottom_right();
    let bottom_right_arc_start = bottom_right + Vector2D::new(0.0, -corner_radius);
    let bottom_right_arc_end = bottom_right + Vector2D::new(-corner_radius, 0.0);
    let bottom_right_curve_center = bottom_right + Vector2D::new(-corner_radius, -corner_radius);
    let bottom_right_p1 = bottom_right_curve_center + Vector2D::new(corner_radius, control_dist);
    let bottom_right_p2 = bottom_right_curve_center + Vector2D::new(control_dist, corner_radius);

    let bottom_left = bounds.bottom_left();
    let bottom_left_arc_start = bottom_left + Vector2D::new(corner_radius, 0.0);
    let bottom_left_arc_end = bottom_left + Vector2D::new(0.0, -corner_radius);
    let bottom_left_curve_center = bottom_left + Vector2D::new(corner_radius, -corner_radius);
    let bottom_left_p1 = bottom_left_curve_center + Vector2D::new(-control_dist, corner_radius);
    let bottom_left_p2 = bottom_left_curve_center + Vector2D::new(-corner_radius, control_dist);

    let mut path_builder = render_context.path_builder().expect("path_builder");
    path_builder
        .move_to(top_left_arc_start)
        .cubic_to(top_left_p1, top_left_p2, top_left_arc_end)
        .line_to(top_right_arc_start)
        .cubic_to(top_right_p1, top_right_p2, top_right_arc_end)
        .line_to(bottom_right_arc_start)
        .cubic_to(bottom_right_p1, bottom_right_p2, bottom_right_arc_end)
        .line_to(bottom_left_arc_start)
        .cubic_to(bottom_left_p1, bottom_left_p2, bottom_left_arc_end)
        .line_to(top_left_arc_start);
    path_builder.build()
}

/// Create a render path for the specified circle.
pub fn path_for_circle(center: Point, radius: Coord, render_context: &mut RenderContext) -> Path {
    let kappa = 4.0 / 3.0 * (std::f32::consts::PI / 8.0).tan();
    let control_dist = kappa * radius;

    let mut path_builder = render_context.path_builder().expect("path_builder");
    let left = center + Vector2D::new(-radius, 0.0);
    let top = center + Vector2D::new(0.0, -radius);
    let right = center + Vector2D::new(radius, 0.0);
    let bottom = center + Vector2D::new(0.0, radius);
    let left_p1 = center + Vector2D::new(-radius, -control_dist);
    let left_p2 = center + Vector2D::new(-control_dist, -radius);
    let top_p1 = center + Vector2D::new(control_dist, -radius);
    let top_p2 = center + Vector2D::new(radius, -control_dist);
    let right_p1 = center + Vector2D::new(radius, control_dist);
    let right_p2 = center + Vector2D::new(control_dist, radius);
    let bottom_p1 = center + Vector2D::new(-control_dist, radius);
    let bottom_p2 = center + Vector2D::new(-radius, control_dist);
    path_builder
        .move_to(left)
        .cubic_to(left_p1, left_p2, top)
        .cubic_to(top_p1, top_p2, right)
        .cubic_to(right_p1, right_p2, bottom)
        .cubic_to(bottom_p1, bottom_p2, left);
    path_builder.build()
}

fn point_for_segment_index(
    index: usize,
    center: Point,
    radius: Coord,
    segment_angle: f32,
) -> Point {
    let angle = index as f32 * segment_angle;
    let x = radius * angle.cos();
    let y = radius * angle.sin();
    center + Vector2D::new(x, y)
}

/// Create a render path for the specified polygon.
pub fn path_for_polygon(
    center: Point,
    radius: Coord,
    segment_count: usize,
    render_context: &mut RenderContext,
) -> Path {
    let segment_angle = (2.0 * std::f32::consts::PI) / segment_count as f32;
    let mut path_builder = render_context.path_builder().expect("path_builder");
    let first_point = point_for_segment_index(0, center, radius, segment_angle);
    path_builder.move_to(first_point);
    for index in 1..segment_count {
        let pt = point_for_segment_index(index, center, radius, segment_angle);
        path_builder.line_to(pt);
    }
    path_builder.line_to(first_point);
    path_builder.build()
}

/// Struct combining a foreground and background color.
#[derive(Hash, Eq, PartialEq, Debug, Clone, Copy)]
pub struct Paint {
    /// Color for foreground painting
    pub fg: Color,
    /// Color for background painting
    pub bg: Color,
}

impl Paint {
    /// Create a paint from a pair of hash codes
    pub fn from_hash_codes(fg: &str, bg: &str) -> Result<Paint, Error> {
        Ok(Paint { fg: Color::from_hash_code(fg)?, bg: Color::from_hash_code(bg)? })
    }
}

/// Struct containing a font and a cache of rendered glyphs.
pub struct FontFace<'a> {
    /// Font.
    pub font: Font<'a>,
}

/// Struct containing font, size and baseline.
#[allow(missing_docs)]
pub struct FontDescription<'a, 'b> {
    pub face: &'a FontFace<'b>,
    pub size: u32,
    pub baseline: i32,
}

#[allow(missing_docs)]
impl<'a> FontFace<'a> {
    pub fn new(data: &'a [u8]) -> Result<FontFace<'a>, Error> {
        let collection = FontCollection::from_bytes(data as &[u8])?;
        let font = collection.into_font()?;
        Ok(FontFace { font: font })
    }
}

pub struct Glyph {
    pub raster: Raster,
    pub bounding_box: Rect,
}

impl Glyph {
    pub fn new(context: &mut RenderContext, face: &FontFace<'_>, size: f32, id: GlyphId) -> Self {
        let mut path_builder = context.path_builder().expect("path_builder");
        let mut bounding_box = Box2D::zero();
        let scale = Scale::uniform(size);

        macro_rules! flip_y {
            ( $p:expr ) => {
                Point::new($p.x, -$p.y)
            };
        }

        let glyph = face.font.glyph(id).scaled(scale);
        if let Some(glyph_box) = glyph.exact_bounding_box() {
            let contours = glyph.shape().expect("shape");
            for contour in contours {
                for segment in &contour.segments {
                    match segment {
                        Segment::Line(line) => {
                            path_builder.move_to(flip_y!(line.p[0]));
                            path_builder.line_to(flip_y!(line.p[1]));
                        }
                        Segment::Curve(curve) => {
                            let p0 = flip_y!(curve.p[0]);
                            let p1 = flip_y!(curve.p[1]);
                            let p2 = flip_y!(curve.p[2]);

                            path_builder.move_to(p0);
                            // TODO: use quad_to when working correctly in spinel backend.
                            path_builder.cubic_to(
                                p0.lerp(p1, 2.0 / 3.0),
                                p2.lerp(p1, 2.0 / 3.0),
                                p2,
                            );
                        }
                    }
                }
            }

            bounding_box = bounding_box.union(&Box2D::new(
                Point::new(glyph_box.min.x, glyph_box.min.y),
                Point::new(glyph_box.max.x, glyph_box.max.y),
            ));
        }

        let path = path_builder.build();
        let mut raster_builder = context.raster_builder().expect("raster_builder");
        raster_builder.add(&path, None);

        Self { raster: raster_builder.build(), bounding_box: bounding_box.to_rect() }
    }
}

pub struct Text {
    pub raster: Raster,
    pub bounding_box: Rect,
}

pub type GlyphMap = BTreeMap<GlyphId, Glyph>;

impl Text {
    pub fn new(
        context: &mut RenderContext,
        text: &str,
        size: f32,
        wrap: usize,
        face: &FontFace<'_>,
        glyphs: &mut GlyphMap,
    ) -> Self {
        let mut bounding_box = Rect::zero();
        let scale = Scale::uniform(size);
        let v_metrics = face.font.v_metrics(scale);
        let mut ascent = v_metrics.ascent;
        let mut raster_union = None;

        for line in wrap_iter(text, wrap) {
            // TODO: adjust vertical alignment of glyphs to match first glyph.
            let y_offset = Vector2D::new(0, ascent as i32);
            let chars = line.chars();
            let mut x: f32 = 0.0;
            let mut last = None;
            for g in face.font.glyphs_for(chars) {
                let g = g.scaled(scale);
                let id = g.id();
                let w = g.h_metrics().advance_width
                    + last.map(|last| face.font.pair_kerning(scale, last, id)).unwrap_or(0.0);

                // Lookup glyph entry in cache.
                // TODO: improve sub pixel placement using a larger cache.
                let position = y_offset + Vector2D::new(x as i32, 0);
                let glyph = glyphs.entry(id).or_insert_with(|| Glyph::new(context, face, size, id));

                // Clone and translate raster.
                let raster = glyph.raster.clone().translate(position);
                raster_union = if let Some(raster_union) = raster_union {
                    Some(raster_union + raster)
                } else {
                    Some(raster)
                };

                // Expand bounding box.
                let glyph_bounding_box = glyph.bounding_box.translate(position.to_f32());
                if bounding_box.is_empty() {
                    bounding_box = glyph_bounding_box;
                } else {
                    bounding_box = bounding_box.union(&glyph_bounding_box);
                }

                x += w;
                last = Some(id);
            }
            ascent += size;
        }

        bounding_box.size.height = size;

        Self { raster: raster_union.expect("raster_union"), bounding_box }
    }
}

#[cfg(test)]
mod tests {
    use super::{GlyphMap, Text};
    use crate::{
        geometry::{Point, UintSize},
        label::make_font_description,
        render::{
            generic::{self, Backend},
            Context as RenderContext, ContextInner,
        },
    };
    use euclid::{approxeq::ApproxEq, Vector2D};
    use fuchsia_async::{self as fasync, Time, TimeoutExt};
    use fuchsia_framebuffer::{sysmem::BufferCollectionAllocator, FrameUsage};

    const DEFAULT_TIMEOUT: fuchsia_zircon::Duration = fuchsia_zircon::Duration::from_seconds(5);

    #[fasync::run_singlethreaded(test)]
    async fn test_text_bounding_box() {
        let size = UintSize::new(800, 800);
        let mut buffer_allocator = BufferCollectionAllocator::new(
            size.width,
            size.height,
            fidl_fuchsia_sysmem::PixelFormatType::Bgra32,
            FrameUsage::Cpu,
            3,
        )
        .expect("BufferCollectionAllocator::new");
        let context_token = buffer_allocator
            .duplicate_token()
            .on_timeout(Time::after(DEFAULT_TIMEOUT), || {
                panic!("Timed out while waiting for duplicate_token")
            })
            .await
            .expect("token");
        let mold_context = generic::Mold::new_context(context_token, size);
        let _buffers_result = buffer_allocator
            .allocate_buffers(true)
            .on_timeout(Time::after(DEFAULT_TIMEOUT), || {
                panic!("Timed out while waiting for sysmem bufers")
            })
            .await;
        let mut render_context = RenderContext { inner: ContextInner::Mold(mold_context) };
        let font_description = make_font_description(20, 0);
        let mut glyphs = GlyphMap::new();
        let text = Text::new(
            &mut render_context,
            "Good Morning",
            20.0,
            200,
            font_description.face,
            &mut glyphs,
        );

        let expected_origin = Point::new(0.0, 3.4487228);
        let expected_size = Vector2D::new(100.486115, 20.0);
        assert!(
            text.bounding_box.origin.approx_eq(&expected_origin),
            "Expected bounding box origin to be close to {} but found {}",
            expected_origin,
            text.bounding_box.origin
        );
        assert!(
            text.bounding_box.size.to_vector().approx_eq(&expected_size),
            "Expected bounding box origin to be close to {} but found {}",
            expected_size,
            text.bounding_box.size
        );
    }
}
