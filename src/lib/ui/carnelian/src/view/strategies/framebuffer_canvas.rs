// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::{
    app::{FrameBufferPtr, MessageInternal, FRAME_COUNT},
    canvas::{Canvas, MappingPixelSink},
    geometry::IntSize,
    input::{self},
    message::Message,
    view::{
        strategies::base::{ViewStrategy, ViewStrategyPtr},
        Canvases, ViewAssistantContext, ViewAssistantPtr, ViewDetails, ViewKey,
    },
};
use anyhow::Error;
use async_trait::async_trait;
use fuchsia_async::{self as fasync};
use fuchsia_framebuffer::{FrameSet, ImageId};
use fuchsia_zircon::{self as zx, ClockId, Duration, Event, HandleBased, Time};
use futures::{
    channel::mpsc::{unbounded, UnboundedSender},
    StreamExt,
};
use std::{cell::RefCell, collections::BTreeMap};

type WaitEvents = BTreeMap<ImageId, Event>;

pub(crate) struct FrameBufferViewStrategy {
    frame_buffer: FrameBufferPtr,
    canvases: Canvases,
    frame_set: FrameSet,
    image_sender: futures::channel::mpsc::UnboundedSender<u64>,
    wait_events: WaitEvents,
    signals_wait_event: bool,
    vsync_phase: Time,
    vsync_interval: Duration,
}

impl FrameBufferViewStrategy {
    pub(crate) async fn new(
        key: ViewKey,
        size: &IntSize,
        pixel_size: u32,
        pixel_format: fuchsia_framebuffer::PixelFormat,
        stride: u32,
        app_sender: UnboundedSender<MessageInternal>,
        frame_buffer: FrameBufferPtr,
        signals_wait_event: bool,
    ) -> Result<ViewStrategyPtr, Error> {
        let mut fb = frame_buffer.borrow_mut();
        fb.allocate_frames(FRAME_COUNT, pixel_format).await?;
        let mut canvases: Canvases = Canvases::new();
        let mut wait_events: WaitEvents = WaitEvents::new();
        let image_ids = fb.get_image_ids();
        image_ids.iter().for_each(|image_id| {
            let frame = fb.get_frame_mut(*image_id);
            let canvas = RefCell::new(Canvas::new(
                *size,
                MappingPixelSink::new(&frame.mapping),
                stride,
                pixel_size,
                frame.image_id,
                frame.image_index,
            ));
            canvases.insert(frame.image_id, canvas);
            if signals_wait_event {
                let wait_event = frame
                    .wait_event
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .expect("duplicate_handle");
                wait_events.insert(frame.image_id, wait_event);
            }
        });
        let (image_sender, mut image_receiver) = unbounded::<u64>();
        // wait for events from the image freed fence to know when an
        // image can prepared.
        fasync::spawn_local(async move {
            while let Some(image_id) = image_receiver.next().await {
                app_sender
                    .unbounded_send(MessageInternal::ImageFreed(key, image_id, 0))
                    .expect("unbounded_send");
            }
        });
        let frame_set = FrameSet::new(image_ids);
        Ok(Box::new(FrameBufferViewStrategy {
            canvases,
            frame_buffer: frame_buffer.clone(),
            frame_set: frame_set,
            image_sender: image_sender,
            wait_events,
            signals_wait_event,
            vsync_phase: Time::get(ClockId::Monotonic),
            vsync_interval: Duration::from_millis(16),
        }))
    }

    fn make_context(
        &mut self,
        view_details: &ViewDetails,
        image_id: ImageId,
    ) -> ViewAssistantContext<'_> {
        let wait_event = if self.signals_wait_event {
            let stored_wait_event = self.wait_events.get(&image_id).expect("wait event");
            Some(stored_wait_event)
        } else {
            None
        };

        let time_now = Time::get(ClockId::Monotonic);
        // |interval_offset| is the offset from |time_now| to the next multiple
        // of vsync interval after vsync phase, possibly negative if in the past.
        let mut interval_offset = Duration::from_nanos(
            (self.vsync_phase.into_nanos() - time_now.into_nanos())
                % self.vsync_interval.into_nanos(),
        );
        // Unless |time_now| is exactly on the interval, adjust forward to the next
        // vsync after |time_now|.
        if interval_offset != Duration::from_nanos(0) && self.vsync_phase < time_now {
            interval_offset += self.vsync_interval;
        }

        let canvas = if image_id != 0 {
            Some(self.canvases.get(&image_id).expect("failed to get canvas in make_context"))
        } else {
            None
        };

        ViewAssistantContext {
            key: view_details.key,
            size: view_details.physical_size,
            metrics: view_details.metrics,
            presentation_time: time_now + interval_offset,
            messages: Vec::new(),
            canvas: canvas,
            buffer_count: Some(self.frame_buffer.borrow().get_frame_count()),
            wait_event: wait_event,
            image_id,
            image_index: 0,
        }
    }
}

#[async_trait(?Send)]
impl ViewStrategy for FrameBufferViewStrategy {
    fn setup(&mut self, view_details: &ViewDetails, view_assistant: &mut ViewAssistantPtr) {
        if let Some(available) = self.frame_set.get_available_image() {
            let framebuffer_context = self.make_context(view_details, available);
            view_assistant
                .setup(&framebuffer_context)
                .unwrap_or_else(|e| panic!("Setup error: {:?}", e));
            self.frame_set.return_image(available);
        }
    }

    async fn update(&mut self, view_details: &ViewDetails, view_assistant: &mut ViewAssistantPtr) {
        if let Some(available) = self.frame_set.get_available_image() {
            let framebuffer_context = self.make_context(view_details, available);
            view_assistant
                .update(&framebuffer_context)
                .unwrap_or_else(|e| panic!("Update error: {:?}", e));
            self.frame_set.mark_prepared(available);
        }
    }

    fn present(&mut self, _view_details: &ViewDetails) {
        if let Some(prepared) = self.frame_set.prepared {
            let mut fb = self.frame_buffer.borrow_mut();
            fb.flush_frame(prepared).unwrap_or_else(|e| panic!("Flush error: {:?}", e));
            fb.present_frame(prepared, Some(self.image_sender.clone()), !self.signals_wait_event)
                .unwrap_or_else(|e| panic!("Present error: {:?}", e));
            self.frame_set.mark_presented(prepared);
        }
    }

    fn handle_scenic_input_event(
        &mut self,
        _view_details: &ViewDetails,
        _view_assistant: &mut ViewAssistantPtr,
        _event: &fidl_fuchsia_ui_input::InputEvent,
    ) -> Vec<Message> {
        unreachable!("Frame buffer strategies should never be used under scenic");
    }

    fn handle_input_event(
        &mut self,
        view_details: &ViewDetails,
        view_assistant: &mut ViewAssistantPtr,
        event: &input::Event,
    ) -> Vec<Message> {
        let mut framebuffer_context = self.make_context(view_details, 0);
        view_assistant
            .handle_input_event(&mut framebuffer_context, &event)
            .unwrap_or_else(|e| eprintln!("handle_new_input_event: {:?}", e));

        framebuffer_context.messages
    }

    fn image_freed(&mut self, image_id: u64, _collection_id: u32) {
        self.frame_set.mark_done_presenting(image_id);
    }

    fn handle_vsync_parameters_changed(&mut self, phase: Time, interval: Duration) {
        self.vsync_phase = phase;
        self.vsync_interval = interval;
    }

    fn handle_vsync_cookie(&mut self, cookie: u64) {
        let mut fb = self.frame_buffer.borrow_mut();
        fb.acknowledge_vsync(cookie).expect("acknowledge_vsync");
    }
}
