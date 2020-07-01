// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use argh::FromArgs;
use carnelian::{
    color::Color,
    drawing::{path_for_circle, path_for_polygon, path_for_rectangle, path_for_rounded_rectangle},
    geometry::{Corners, IntPoint},
    input::{self},
    make_app_assistant,
    render::{
        BlendMode, Composition, Context as RenderContext, Fill, FillRule, Layer, PreClear, Raster,
        RenderExt, Style,
    },
    App, AppAssistant, Coord, Point, Rect, RenderOptions, Size, ViewAssistant,
    ViewAssistantContext, ViewAssistantPtr, ViewKey,
};
use euclid::default::Vector2D;
use fuchsia_trace::duration;
use fuchsia_zircon::{AsHandleRef, Event, Signals};
use rand::{thread_rng, Rng};
use std::{collections::BTreeMap, mem};

fn make_bounds(context: &ViewAssistantContext) -> Rect {
    Rect::new(Point::zero(), context.size)
}

/// Shapes
#[derive(Clone, Debug, FromArgs)]
#[argh(name = "shapes")]
struct Args {
    /// use spinel
    #[argh(switch, short = 's')]
    use_spinel: bool,
}

#[derive(Default)]
struct ShapeDropAppAssistant;

impl AppAssistant for ShapeDropAppAssistant {
    fn setup(&mut self) -> Result<(), Error> {
        Ok(())
    }

    fn create_view_assistant(&mut self, _: ViewKey) -> Result<ViewAssistantPtr, Error> {
        Ok(Box::new(ShapeDropViewAssistant::new()?))
    }

    fn get_render_options(&self) -> RenderOptions {
        let args: Args = argh::from_env();
        RenderOptions { use_spinel: args.use_spinel, ..RenderOptions::default() }
    }
}

struct ShapeAnimator {
    shape_type: ShapeType,
    color: Color,
    location: Point,
    accel: Vector2D<f32>,
    velocity: Vector2D<f32>,
    running: bool,
}

impl ShapeAnimator {
    pub fn new(touch_handler: TouchHandler, location: Point) -> ShapeAnimator {
        ShapeAnimator {
            shape_type: touch_handler.shape_type,
            color: touch_handler.color,
            location: location,
            accel: Vector2D::new(0.0, 1.0),
            velocity: Vector2D::zero(),
            running: true,
        }
    }

    pub fn animate(&mut self, bounds: &Rect) {
        self.location += self.velocity;
        self.velocity += self.accel;
        if self.location.y > bounds.max_y() {
            self.running = false;
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
enum ShapeType {
    Rectangle,
    RoundedRectangle,
    Circle,
    Hexagon,
    Triangle,
    Octagon,
    LastShapeType,
}

#[derive(Debug)]
struct TouchHandler {
    location: Point,
    shape_type: ShapeType,
    color: Color,
}

fn random_color_element() -> u8 {
    let mut rng = thread_rng();
    let e: u8 = rng.gen_range(0, 128);
    e + 128
}

fn random_color() -> Color {
    Color {
        r: random_color_element(),
        g: random_color_element(),
        b: random_color_element(),
        a: 0xff,
    }
}

impl TouchHandler {
    pub fn new() -> TouchHandler {
        let mut rng = thread_rng();
        let shape_type = match rng.gen_range(0, ShapeType::LastShapeType as usize) {
            0 => ShapeType::Rectangle,
            1 => ShapeType::RoundedRectangle,
            2 => ShapeType::Hexagon,
            3 => ShapeType::Triangle,
            4 => ShapeType::Octagon,
            _ => ShapeType::Circle,
        };
        let color = random_color();
        TouchHandler { location: Point::zero(), color, shape_type }
    }

    fn update(&mut self, context: &mut ViewAssistantContext, location: &IntPoint) {
        let touch_point = location.to_f32();
        let bounds = make_bounds(context);
        self.location =
            Point::new(touch_point.x, touch_point.y).clamp(bounds.origin, bounds.bottom_right());
    }
}

fn raster_for_rectangle(bounds: &Rect, render_context: &mut RenderContext) -> Raster {
    let mut raster_builder = render_context.raster_builder().expect("raster_builder");
    raster_builder.add(&path_for_rectangle(bounds, render_context), None);
    raster_builder.build()
}

fn raster_for_rounded_rectangle(
    bounds: &Rect,
    corner_radius: Coord,
    render_context: &mut RenderContext,
) -> Raster {
    let path = path_for_rounded_rectangle(bounds, corner_radius, render_context);
    let mut raster_builder = render_context.raster_builder().expect("raster_builder");
    raster_builder.add(&path, None);
    raster_builder.build()
}

fn raster_for_circle(center: Point, radius: Coord, render_context: &mut RenderContext) -> Raster {
    let path = path_for_circle(center, radius, render_context);
    let mut raster_builder = render_context.raster_builder().expect("raster_builder");
    raster_builder.add(&path, None);
    raster_builder.build()
}

fn raster_for_polygon(
    center: Point,
    radius: Coord,
    segment_count: usize,
    render_context: &mut RenderContext,
) -> Raster {
    let path = path_for_polygon(center, radius, segment_count, render_context);
    let mut raster_builder = render_context.raster_builder().expect("raster_builder");
    raster_builder.add(&path, None);
    raster_builder.build()
}

struct ShapeDropViewAssistant {
    renderings: BTreeMap<u64, Rendering>,
    touch_handlers: BTreeMap<input::pointer::PointerId, TouchHandler>,
    animators: Vec<ShapeAnimator>,
    background_color: Color,
    composition: Composition,
    shapes: BTreeMap<ShapeType, Raster>,
}

impl ShapeDropViewAssistant {
    fn new() -> Result<ShapeDropViewAssistant, Error> {
        let background_color = Color::from_hash_code("#2F4F4F")?;
        let composition = Composition::new(background_color);
        Ok(ShapeDropViewAssistant {
            renderings: BTreeMap::new(),
            composition,
            background_color,
            touch_handlers: BTreeMap::new(),
            animators: Vec::new(),
            shapes: BTreeMap::new(),
        })
    }

    fn start_animating(
        &mut self,
        context: &mut ViewAssistantContext,
        location: &IntPoint,
        pointer_id: &input::pointer::PointerId,
    ) {
        if let Some(handler) = self.touch_handlers.remove(&pointer_id) {
            let bounds = make_bounds(context);
            let touch_point = location.to_f32();
            let location = Point::new(touch_point.x, touch_point.y)
                .clamp(bounds.origin, bounds.bottom_right());
            let animator = ShapeAnimator::new(handler, Point::new(location.x, location.y));
            self.animators.push(animator);
        }
    }

    fn setup_shapes(&mut self, render_context: &mut RenderContext) {
        if self.shapes.is_empty() {
            let size = Size::new(60.0, 60.0);
            let origin = Point::zero() - size.to_vector() / 2.0;
            let shape_bounds = Rect::new(origin, size);
            let raster = raster_for_rectangle(&shape_bounds, render_context);
            self.shapes.insert(ShapeType::Rectangle, raster);
            let raster = raster_for_circle(Point::zero(), size.width / 2.0, render_context);
            self.shapes.insert(ShapeType::Circle, raster);
            let raster =
                raster_for_rounded_rectangle(&shape_bounds, size.width * 0.25, render_context);
            self.shapes.insert(ShapeType::RoundedRectangle, raster);
            let raster = raster_for_polygon(Point::zero(), size.width / 2.0, 6, render_context);
            self.shapes.insert(ShapeType::Hexagon, raster);
            let raster = raster_for_polygon(Point::zero(), size.width / 2.0, 3, render_context);
            self.shapes.insert(ShapeType::Triangle, raster);
            let raster = raster_for_polygon(Point::zero(), size.width / 2.0, 8, render_context);
            self.shapes.insert(ShapeType::Octagon, raster);
        }
    }
}

#[derive(Debug)]
struct ShapeAtPosition {
    shape_type: ShapeType,
    translation: Vector2D<i32>,
}

struct Rendering {
    size: Size,
    previous_shapes: Vec<ShapeAtPosition>,
}

impl Rendering {
    fn new() -> Rendering {
        Rendering { previous_shapes: Vec::new(), size: Size::zero() }
    }
}

impl ViewAssistant for ShapeDropViewAssistant {
    fn render(
        &mut self,
        render_context: &mut RenderContext,
        ready_event: Event,
        context: &ViewAssistantContext,
    ) -> Result<(), Error> {
        duration!("gfx", "ShapeDropViewAssistant::render");

        let background_color = self.background_color;

        self.setup_shapes(render_context);
        let shapes = self.shapes.clone();

        let mut animators = Vec::new();
        mem::swap(&mut animators, &mut self.animators);
        let bounds = make_bounds(context);
        for animator in animators.iter_mut() {
            animator.animate(&bounds);
        }

        let image_id = context.image_id;

        let rendering = self.renderings.entry(image_id).or_insert_with(|| Rendering::new());

        let pre_clear = if context.size != rendering.size {
            rendering.size = context.size;
            rendering.previous_shapes.clear();
            Some(PreClear { color: background_color })
        } else {
            None
        };

        let mut previous_shapes = Vec::new();
        mem::swap(&mut rendering.previous_shapes, &mut previous_shapes);

        let clear_layers = previous_shapes.into_iter().map(|previous_shape| {
            let raster = shapes.get(&previous_shape.shape_type).expect("shape");
            let translation = previous_shape.translation;
            let translated_raster = raster.clone().translate(translation);
            Layer {
                raster: translated_raster,
                style: Style {
                    fill_rule: FillRule::WholeTile,
                    fill: Fill::Solid(background_color),
                    blend_mode: BlendMode::Over,
                },
            }
        });

        let (mut animator_positioned_shapes, animation_layers): (Vec<_>, Vec<_>) = animators
            .iter()
            .map(|animator| {
                let raster = shapes.get(&animator.shape_type).expect("shape");
                let translation = animator.location.to_vector().to_i32();
                let translated_raster = raster.clone().translate(translation);
                (
                    ShapeAtPosition { shape_type: animator.shape_type, translation: translation },
                    Layer {
                        raster: translated_raster,
                        style: Style {
                            fill_rule: FillRule::NonZero,
                            fill: Fill::Solid(animator.color),
                            blend_mode: BlendMode::Over,
                        },
                    },
                )
            })
            .unzip();

        let (mut touch_positioned_shapes, touch_layers): (Vec<_>, Vec<_>) = self
            .touch_handlers
            .iter()
            .map(|(_, touch_handler)| {
                let raster = shapes.get(&touch_handler.shape_type).expect("shape");
                let translation = touch_handler.location.to_vector().to_i32();
                let translated_raster = raster.clone().translate(translation);
                (
                    ShapeAtPosition {
                        shape_type: touch_handler.shape_type,
                        translation: translation,
                    },
                    Layer {
                        raster: translated_raster,
                        style: Style {
                            fill_rule: FillRule::NonZero,
                            fill: Fill::Solid(touch_handler.color),
                            blend_mode: BlendMode::Over,
                        },
                    },
                )
            })
            .unzip();

        let layers = touch_layers.into_iter().chain(animation_layers).chain(clear_layers);

        self.composition.replace(.., layers);

        animators.retain(|animator| animator.running);
        mem::swap(&mut animators, &mut self.animators);

        rendering.previous_shapes.append(&mut touch_positioned_shapes);
        rendering.previous_shapes.append(&mut animator_positioned_shapes);

        let image = render_context.get_current_image(context);
        let ext = RenderExt { pre_clear, ..Default::default() };
        render_context.render(&self.composition, None, image, &ext);
        ready_event.as_handle_ref().signal(Signals::NONE, Signals::EVENT_SIGNALED)?;

        context.request_render();

        Ok(())
    }

    fn handle_pointer_event(
        &mut self,
        context: &mut ViewAssistantContext,
        _event: &input::Event,
        pointer_event: &input::pointer::Event,
    ) -> Result<(), Error> {
        match &pointer_event.phase {
            input::pointer::Phase::Down(touch_location) => {
                let mut t = TouchHandler::new();
                t.update(context, &touch_location);
                self.touch_handlers.insert(pointer_event.pointer_id.clone(), t);
            }
            input::pointer::Phase::Moved(touch_location) => {
                if let Some(handler) = self.touch_handlers.get_mut(&pointer_event.pointer_id) {
                    handler.update(context, &touch_location);
                }
            }
            input::pointer::Phase::Up => {
                let end_location =
                    if let Some(handler) = self.touch_handlers.get_mut(&pointer_event.pointer_id) {
                        handler.location.to_i32()
                    } else {
                        IntPoint::zero()
                    };
                self.start_animating(context, &end_location, &pointer_event.pointer_id);
            }
            _ => (),
        }
        Ok(())
    }
}

fn main() -> Result<(), Error> {
    fuchsia_trace_provider::trace_provider_create_with_fdio();
    App::run(make_app_assistant::<ShapeDropAppAssistant>())
}
