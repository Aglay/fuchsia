// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::{
    input,
    message::Message,
    view::{ViewAssistantPtr, ViewDetails},
};
use async_trait::async_trait;
use fuchsia_zircon::{Duration, Time};

#[async_trait(?Send)]
pub(crate) trait ViewStrategy {
    fn setup(&mut self, _view_details: &ViewDetails, _view_assistant: &mut ViewAssistantPtr);
    async fn update(&mut self, view_details: &ViewDetails, view_assistant: &mut ViewAssistantPtr);
    fn present(&mut self, view_details: &ViewDetails);
    fn present_done(
        &mut self,
        _view_details: &ViewDetails,
        _view_assistant: &mut ViewAssistantPtr,
    ) {
    }
    fn handle_scenic_input_event(
        &mut self,
        _view_details: &ViewDetails,
        _view_assistant: &mut ViewAssistantPtr,
        _: &fidl_fuchsia_ui_input::InputEvent,
    ) -> Vec<Message>;

    fn handle_input_event(
        &mut self,
        _view_details: &ViewDetails,
        _view_assistant: &mut ViewAssistantPtr,
        _: &input::Event,
    ) -> Vec<Message> {
        Vec::new()
    }

    fn image_freed(&mut self, _image_id: u64, _collection_id: u32) {}

    fn handle_vsync_parameters_changed(&mut self, _phase: Time, _interval: Duration) {}
}

pub(crate) type ViewStrategyPtr = Box<dyn ViewStrategy>;
