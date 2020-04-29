// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::{
    app::MessageInternal,
    canvas::{Canvas, MappingPixelSink},
    geometry::{IntSize, UintSize},
    input::ScenicInputHandler,
    message::Message,
    view::{
        scenic_present, scenic_present_done,
        strategies::base::{ViewStrategy, ViewStrategyPtr},
        Canvases, ScenicResources, ViewAssistantContext, ViewAssistantPtr, ViewDetails,
    },
};
use anyhow::{Context, Error, Result};
use async_trait::async_trait;
use fidl::endpoints::create_endpoints;
use fidl_fuchsia_images::{
    AlphaFormat, ColorSpace, ImageInfo, ImagePipe2Marker, ImagePipe2Proxy, MemoryType, PixelFormat,
    Tiling, Transform,
};
use fidl_fuchsia_sysmem::ImageFormat2;
use fidl_fuchsia_ui_views::{ViewRef, ViewRefControl, ViewToken};
use fuchsia_async::{self as fasync, OnSignals};
use fuchsia_framebuffer::{
    sysmem::{minimum_row_bytes, BufferCollectionAllocator},
    FrameSet, FrameUsage, ImageId,
};
use fuchsia_scenic::{Image, ImagePipe2, Material, Memory, Rectangle, SessionPtr, ShapeNode};
use fuchsia_zircon::{self as zx, ClockId, Event, HandleBased, Signals, Time, Vmo};
use futures::channel::mpsc::UnboundedSender;
use mapped_vmo::Mapping;
use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
    iter,
    sync::Arc,
};

pub(crate) type Vmos = BTreeMap<u64, Vmo>;

// TODO: Remove this workaround once ImagePipe2 works in Femu.
// https://bugs.fuchsia.dev/p/fuchsia/issues/detail?id=6469

enum PlumberTools {
    SysMem {},
    Vmo { vmos: Vmos },
}

struct Plumber {
    pub size: UintSize,
    pub buffer_count: usize,
    pub collection_id: u32,
    pub first_image_id: u64,
    pub frame_set: FrameSet,
    pub canvases: Canvases,
    pub tools: PlumberTools,
}

impl Plumber {
    async fn new(
        size: UintSize,
        pixel_format: fidl_fuchsia_sysmem::PixelFormatType,
        buffer_count: usize,
        collection_id: u32,
        first_image_id: u64,
        image_pipe_client: &mut ImagePipe2Proxy,
    ) -> Result<Plumber, Error> {
        let mut buffer_allocator = BufferCollectionAllocator::new(
            size.width,
            size.height,
            pixel_format,
            FrameUsage::Cpu,
            buffer_count,
        )?;
        let image_pipe_token = buffer_allocator.duplicate_token().await?;
        image_pipe_client.add_buffer_collection(collection_id, image_pipe_token)?;
        let buffers_result = buffer_allocator.allocate_buffers(true).await;
        let mut canvases: Canvases = Canvases::new();
        let mut image_ids = BTreeSet::new();
        let tools = if let Ok(buffers) = buffers_result {
            let linear_stride_bytes =
                minimum_row_bytes(buffers.settings.image_format_constraints, size.width)?;
            let buffer_size = linear_stride_bytes * size.height;
            let mut index = 0;
            for buffer in &buffers.buffers[0..buffers.buffer_count as usize] {
                let image_id = index + first_image_id;
                image_ids.insert(image_id);
                let vmo = buffer.vmo.as_ref().expect("vmo");

                let mapping = Mapping::create_from_vmo(
                    vmo,
                    buffer_size as usize,
                    zx::VmarFlags::PERM_READ
                        | zx::VmarFlags::PERM_WRITE
                        | zx::VmarFlags::MAP_RANGE
                        | zx::VmarFlags::REQUIRE_NON_RESIZABLE,
                )
                .context("Frame::new() Mapping::create_from_vmo failed")
                .expect("mapping");
                let canvas = RefCell::new(Canvas::new(
                    IntSize::new(size.width as i32, size.height as i32),
                    MappingPixelSink::new(&Arc::new(mapping)),
                    linear_stride_bytes,
                    4,
                    image_id,
                    index as u32,
                ));
                canvases.insert(image_id, canvas);
                let uindex = index as u32;
                image_pipe_client
                    .add_image(
                        image_id as u32,
                        collection_id,
                        uindex,
                        &mut make_image_format(size.width, size.height, pixel_format),
                    )
                    .expect("add_image");
                index += 1;
            }
            PlumberTools::SysMem {}
        } else {
            let linear_stride_bytes = size.width * 4;
            let buffer_size = linear_stride_bytes * size.height;
            let mut vmos = Vmos::new();
            for index in 0..buffer_count as u64 {
                let image_id = index + first_image_id;
                image_ids.insert(image_id);
                let vmo = Vmo::create(buffer_size.into())?;
                let mapping = Mapping::create_from_vmo(
                    &vmo,
                    buffer_size as usize,
                    zx::VmarFlags::PERM_READ
                        | zx::VmarFlags::PERM_WRITE
                        | zx::VmarFlags::MAP_RANGE
                        | zx::VmarFlags::REQUIRE_NON_RESIZABLE,
                )
                .context("Frame::new() Mapping::create_from_vmo failed")
                .expect("mapping");
                let canvas = RefCell::new(Canvas::new(
                    IntSize::new(size.width as i32, size.height as i32),
                    MappingPixelSink::new(&Arc::new(mapping)),
                    linear_stride_bytes,
                    4,
                    image_id,
                    index as u32,
                ));
                canvases.insert(image_id, canvas);
                vmos.insert(image_id, vmo);
            }
            PlumberTools::Vmo { vmos }
        };
        let frame_set = FrameSet::new(image_ids);
        Ok(Plumber {
            size,
            buffer_count,
            collection_id,
            first_image_id,
            frame_set,
            canvases,
            tools,
        })
    }

    pub fn enter_retirement(&mut self, image_pipe_client: &mut ImagePipe2Proxy) {
        match &self.tools {
            PlumberTools::Vmo { .. } => (),
            PlumberTools::SysMem { .. } => {
                for image_id in self.first_image_id..self.first_image_id + self.buffer_count as u64
                {
                    image_pipe_client.remove_image(image_id as u32).unwrap_or_else(|err| {
                        eprintln!("image_pipe_client.remove_image {} failed with {}", image_id, err)
                    });
                }
                image_pipe_client.remove_buffer_collection(self.collection_id).unwrap_or_else(
                    |err| {
                        eprintln!(
                            "image_pipe_client.remove_buffer_collection {} failed with {}",
                            self.collection_id, err
                        )
                    },
                );
            }
        }
    }
}

const SCENIC_CANVAS_BUFFER_COUNT: usize = 3;

pub(crate) struct ScenicCanvasViewStrategy {
    #[allow(unused)]
    scenic_resources: ScenicResources,
    content_node: ShapeNode,
    content_material: Material,
    next_buffer_collection: u32,
    image_pipe: ImagePipe2,
    image_pipe_client: ImagePipe2Proxy,
    plumber: Option<Plumber>,
    retiring_plumbers: Vec<Plumber>,
    next_image_id: u64,
    input_handler: ScenicInputHandler,
}

impl ScenicCanvasViewStrategy {
    pub(crate) async fn new(
        session: &SessionPtr,
        view_token: ViewToken,
        control_ref: ViewRefControl,
        view_ref: ViewRef,
        app_sender: UnboundedSender<MessageInternal>,
    ) -> Result<ViewStrategyPtr, Error> {
        let scenic_resources =
            ScenicResources::new(session, view_token, control_ref, view_ref, app_sender);
        let (image_pipe_client, server_end) = create_endpoints::<ImagePipe2Marker>()?;
        let image_pipe = ImagePipe2::new(session.clone(), server_end);
        let content_material = Material::new(session.clone());
        let content_node = ShapeNode::new(session.clone());
        content_node.set_material(&content_material);
        scenic_resources.root_node.add_child(&content_node);
        let image_pipe_client = image_pipe_client.into_proxy()?;
        session.lock().flush();
        let strat = ScenicCanvasViewStrategy {
            scenic_resources,
            image_pipe_client,
            image_pipe,
            content_node,
            content_material,
            plumber: None,
            retiring_plumbers: Vec::new(),
            next_buffer_collection: 1,
            next_image_id: 1,
            input_handler: ScenicInputHandler::new(),
        };
        Ok(Box::new(strat))
    }

    fn make_view_assistant_context<'a>(
        view_details: &ViewDetails,
        image_id: ImageId,
        canvas: Option<&'a RefCell<Canvas<MappingPixelSink>>>,
    ) -> ViewAssistantContext<'a> {
        ViewAssistantContext {
            key: view_details.key,
            size: view_details.physical_size,
            metrics: view_details.metrics,
            presentation_time: Time::get(ClockId::Monotonic),
            messages: Vec::new(),
            canvas: canvas,
            buffer_count: None,
            wait_event: None,
            image_id,
            image_index: 0,
        }
    }

    async fn create_plumber(&mut self, size: UintSize) -> Result<(), Error> {
        let buffer_collection_id = self.next_buffer_collection;
        self.next_buffer_collection = self.next_buffer_collection.wrapping_add(1);
        let next_image_id = self.next_image_id;
        self.next_image_id = self.next_image_id.wrapping_add(SCENIC_CANVAS_BUFFER_COUNT as u64);
        self.plumber = Some(
            Plumber::new(
                size.to_u32(),
                fidl_fuchsia_sysmem::PixelFormatType::Bgra32,
                SCENIC_CANVAS_BUFFER_COUNT,
                buffer_collection_id,
                next_image_id,
                &mut self.image_pipe_client,
            )
            .await
            .expect("VmoPlumber::new"),
        );
        Ok(())
    }
}

fn make_image_format(
    width: u32,
    height: u32,
    pixel_format: fidl_fuchsia_sysmem::PixelFormatType,
) -> ImageFormat2 {
    ImageFormat2 {
        bytes_per_row: width * 4,
        coded_height: height,
        coded_width: width,
        color_space: fidl_fuchsia_sysmem::ColorSpace {
            type_: fidl_fuchsia_sysmem::ColorSpaceType::Srgb,
        },
        display_height: height,
        display_width: width,
        has_pixel_aspect_ratio: false,
        layers: 1,
        pixel_aspect_ratio_height: 1,
        pixel_aspect_ratio_width: 1,
        pixel_format: fidl_fuchsia_sysmem::PixelFormat {
            type_: pixel_format,
            has_format_modifier: true,
            format_modifier: fidl_fuchsia_sysmem::FormatModifier {
                value: fidl_fuchsia_sysmem::FORMAT_MODIFIER_LINEAR,
            },
        },
    }
}

#[async_trait(?Send)]
impl ViewStrategy for ScenicCanvasViewStrategy {
    fn setup(&mut self, view_details: &ViewDetails, view_assistant: &mut ViewAssistantPtr) {
        let canvas_context =
            ScenicCanvasViewStrategy::make_view_assistant_context(view_details, 0, None);
        view_assistant.setup(&canvas_context).unwrap_or_else(|e| panic!("Setup error: {:?}", e));
    }

    async fn update(&mut self, view_details: &ViewDetails, view_assistant: &mut ViewAssistantPtr) {
        let size = view_details.logical_size.floor().to_u32();
        if size.width > 0 && size.height > 0 {
            let session = self.scenic_resources.session.clone();
            if self.plumber.is_none() {
                self.create_plumber(size).await.expect("create_plumber");
            } else {
                let current_size = self.plumber.as_ref().expect("plumber").size;
                if current_size != size {
                    let retired_plumber = self.plumber.take().expect("plumber");
                    self.retiring_plumbers.push(retired_plumber);
                    self.create_plumber(size).await.expect("create_plumber");
                }
            }
            let plumber = self.plumber.as_mut().expect("plumber");
            let center_x = view_details.physical_size.width * 0.5;
            let center_y = view_details.physical_size.height * 0.5;
            self.content_node.set_translation(center_x, center_y, -0.1);
            let rectangle = Rectangle::new(
                self.scenic_resources.session.clone(),
                view_details.physical_size.width,
                view_details.physical_size.height,
            );
            if let Some(available) = plumber.frame_set.get_available_image() {
                let canvas = plumber.canvases.get(&available).expect("canvas");
                self.content_node.set_shape(&rectangle);
                let canvas_context = ScenicCanvasViewStrategy::make_view_assistant_context(
                    view_details,
                    available,
                    Some(canvas),
                );
                view_assistant
                    .update(&canvas_context)
                    .unwrap_or_else(|e| panic!("Update error: {:?}", e));
                plumber.frame_set.mark_prepared(available);
                let wait_event = Event::create().expect("Event.create");
                let local_event =
                    wait_event.duplicate_handle(zx::Rights::SAME_RIGHTS).expect("duplicate_handle");
                let app_sender = self.scenic_resources.app_sender.clone();
                let key = view_details.key;
                let collection_id = plumber.collection_id;
                fasync::spawn_local(async move {
                    let signals = OnSignals::new(&local_event, Signals::EVENT_SIGNALED);
                    signals.await.expect("to wait");
                    app_sender
                        .unbounded_send(MessageInternal::ImageFreed(key, available, collection_id))
                        .expect("unbounded_send");
                });
                match &plumber.tools {
                    PlumberTools::Vmo { vmos } => {
                        session.lock().add_release_fence(wait_event);
                        let vmo = vmos.get(&available).expect("vmo");
                        let vmo = vmo
                            .duplicate_handle(zx::Rights::SAME_RIGHTS)
                            .expect("duplicating buffer vmo");

                        let linear_stride_bytes = size.width * 4;
                        let buffer_size = linear_stride_bytes * size.height;
                        let memory = Memory::new(
                            session.clone(),
                            vmo,
                            buffer_size.into(),
                            MemoryType::HostMemory,
                        );
                        let info = ImageInfo {
                            width: size.width,
                            height: size.height,
                            stride: linear_stride_bytes,
                            transform: Transform::Normal,
                            alpha_format: AlphaFormat::Opaque,
                            pixel_format: PixelFormat::Bgra8,
                            color_space: ColorSpace::Srgb,
                            tiling: Tiling::Linear,
                        };
                        let image = Image::new(&memory, 0, info);
                        self.content_material.set_texture(Some(&image));
                    }
                    PlumberTools::SysMem { .. } => {
                        self.content_material.set_texture_resource(Some(&self.image_pipe));
                        self.image_pipe_client
                            .present_image(
                                canvas.borrow().id as u32,
                                0,
                                &mut iter::empty(),
                                &mut iter::once(wait_event),
                            )
                            .await
                            .expect("present_image");
                    }
                }
                plumber.frame_set.mark_presented(available);
            }
        }
    }

    fn present(&mut self, view_details: &ViewDetails) {
        scenic_present(&mut self.scenic_resources, view_details.key);
    }

    fn present_done(
        &mut self,
        _view_details: &ViewDetails,
        _view_assistant: &mut ViewAssistantPtr,
    ) {
        scenic_present_done(&mut self.scenic_resources);
    }

    fn handle_scenic_input_event(
        &mut self,
        view_details: &ViewDetails,
        view_assistant: &mut ViewAssistantPtr,
        event: &fidl_fuchsia_ui_input::InputEvent,
    ) -> Vec<Message> {
        let events = self.input_handler.handle_scenic_input_event(&view_details.metrics, &event);

        let mut canvas_context =
            ScenicCanvasViewStrategy::make_view_assistant_context(view_details, 0, None);
        for input_event in events {
            view_assistant
                .handle_input_event(&mut canvas_context, &input_event)
                .unwrap_or_else(|e| eprintln!("handle_event: {:?}", e));
        }

        canvas_context.messages
    }

    fn image_freed(&mut self, image_id: u64, collection_id: u32) {
        if let Some(plumber) = self.plumber.as_mut() {
            if plumber.collection_id == collection_id {
                plumber.frame_set.mark_done_presenting(image_id);
                return;
            }
        }

        for retired_plumber in &mut self.retiring_plumbers {
            if retired_plumber.collection_id == collection_id {
                retired_plumber.frame_set.mark_done_presenting(image_id);
                if retired_plumber.frame_set.no_images_in_use() {
                    retired_plumber.enter_retirement(&mut self.image_pipe_client);
                }
            }
        }

        self.retiring_plumbers.retain(|plumber| !plumber.frame_set.no_images_in_use());
    }
}
