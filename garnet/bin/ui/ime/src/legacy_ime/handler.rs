// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::state::{get_point, get_range, ImeState};
use super::{
    HID_USAGE_KEY_BACKSPACE, HID_USAGE_KEY_DELETE, HID_USAGE_KEY_ENTER, HID_USAGE_KEY_LEFT,
    HID_USAGE_KEY_RIGHT,
};
use crate::ime_service::ImeService;
use crate::index_convert as idx;
use failure::ResultExt;
use fidl::endpoints::RequestStream;
use fidl_fuchsia_ui_input as uii;
use fidl_fuchsia_ui_input::InputMethodEditorRequest as ImeReq;
use fidl_fuchsia_ui_text as txt;
use fuchsia_syslog::{fx_log_err, fx_log_warn};
use futures::prelude::*;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::{Arc, Weak};

/// A service that talks to a text field, providing it edits and cursor state updates
/// in response to user input.
#[derive(Clone)]
pub struct Ime(Arc<Mutex<ImeState>>);

impl Ime {
    pub fn new<I: 'static + uii::InputMethodEditorClientProxyInterface>(
        keyboard_type: uii::KeyboardType,
        action: uii::InputMethodAction,
        initial_state: uii::TextInputState,
        client: I,
        ime_service: ImeService,
    ) -> Ime {
        let state = ImeState {
            text_state: initial_state,
            client: Box::new(client),
            keyboard_type,
            action,
            ime_service,
            revision: 0,
            next_text_point_id: 0,
            text_points: HashMap::new(),
            input_method: None,
            transaction_changes: Vec::new(),
            transaction_revision: None,
        };
        Ime(Arc::new(Mutex::new(state)))
    }

    pub fn downgrade(&self) -> Weak<Mutex<ImeState>> {
        Arc::downgrade(&self.0)
    }

    pub fn upgrade(weak: &Weak<Mutex<ImeState>>) -> Option<Ime> {
        weak.upgrade().map(|arc| Ime(arc))
    }

    pub fn bind_text_field(&self, mut stream: txt::TextFieldRequestStream) {
        let control_handle = stream.control_handle();
        {
            let mut state = self.0.lock();
            let res = control_handle.send_on_update(&mut state.as_text_field_state());
            if let Err(e) = res {
                fx_log_err!("{}", e);
            } else {
                state.input_method = Some(control_handle);
            }
        }
        let mut self_clone = self.clone();
        fuchsia_async::spawn(
            async move {
                while let Some(msg) = await!(stream.try_next())
                    .context("error reading value from text field request stream")?
                {
                    if let Err(e) = self_clone.handle_text_field_msg(msg) {
                        fx_log_err!("error when replying to TextFieldRequest: {}", e);
                    }
                }
                Ok(())
            }
                .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
        );
    }

    pub fn bind_ime(&self, chan: fuchsia_async::Channel) {
        let self_clone = self.clone();
        let self_clone_2 = self.clone();
        fuchsia_async::spawn(
            async move {
                let mut stream = uii::InputMethodEditorRequestStream::from_channel(chan);
                while let Some(msg) = await!(stream.try_next())
                    .context("error reading value from IME request stream")?
                {
                    self_clone.handle_ime_message(msg);
                }
                Ok(())
            }
                .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e))
                .then(async move |()| {
                    // this runs when IME stream closes
                    // clone to ensure we only hold one lock at a time
                    let ime_service = self_clone_2.0.lock().ime_service.clone();
                    ime_service.update_keyboard_visibility_from_ime(&self_clone_2.0, false);
                }),
        );
    }

    /// Handles a TextFieldRequest, returning a FIDL error if one occurred when sending a reply.
    fn handle_text_field_msg(&mut self, msg: txt::TextFieldRequest) -> Result<(), fidl::Error> {
        let mut ime_state = self.0.lock();
        match msg {
            txt::TextFieldRequest::PositionOffset { old_position, offset, revision, responder } => {
                if revision != ime_state.revision {
                    return responder.send(&mut txt::Position { id: 0 }, txt::Error::BadRevision);
                }
                let old_char_index = if let Some(v) =
                    get_point(&ime_state.text_points, &old_position).and_then(|old_byte_index| {
                        idx::byte_to_char(&ime_state.text_state.text, old_byte_index)
                    }) {
                    v
                } else {
                    return responder.send(&mut txt::Position { id: 0 }, txt::Error::BadRequest);
                };
                let new_char_index = (old_char_index as i64 + offset)
                    .max(0)
                    .min(ime_state.text_state.text.chars().count() as i64);
                // ok to .expect() here, since char_to_byte can only fail if new_char_index is out of the char indices
                let new_byte_index = idx::char_to_byte(&ime_state.text_state.text, new_char_index)
                    .expect("did not expect character to fail");
                let mut new_point = ime_state.new_point(new_byte_index);
                return responder.send(&mut new_point, txt::Error::Ok);
            }
            txt::TextFieldRequest::Distance { range, revision, responder } => {
                if revision != ime_state.revision {
                    return responder.send(0, txt::Error::BadRevision);
                }
                let (byte_start, byte_end) = match get_range(&ime_state.text_points, &range, false)
                {
                    Some(v) => v,
                    None => {
                        return responder.send(0, txt::Error::BadRequest);
                    }
                };
                let (char_start, char_end) = match (
                    idx::byte_to_char(&ime_state.text_state.text, byte_start),
                    idx::byte_to_char(&ime_state.text_state.text, byte_end),
                ) {
                    (Some(a), Some(b)) => (a, b),
                    _ => {
                        return responder.send(0, txt::Error::BadRequest);
                    }
                };
                return responder.send(char_end as i64 - char_start as i64, txt::Error::Ok);
            }
            txt::TextFieldRequest::Contents { range, revision, responder } => {
                if revision != ime_state.revision {
                    return responder.send(
                        "",
                        &mut txt::Position { id: 0 },
                        txt::Error::BadRevision,
                    );
                }
                match get_range(&ime_state.text_points, &range, true) {
                    Some((start, end)) => {
                        let mut start_point = ime_state.new_point(start);
                        match ime_state.text_state.text.get(start..end) {
                            Some(contents) => {
                                return responder.send(contents, &mut start_point, txt::Error::Ok);
                            }
                            None => {
                                return responder.send(
                                    "",
                                    &mut txt::Position { id: 0 },
                                    txt::Error::BadRequest,
                                );
                            }
                        }
                    }
                    None => {
                        return responder.send(
                            "",
                            &mut txt::Position { id: 0 },
                            txt::Error::BadRequest,
                        );
                    }
                }
            }
            txt::TextFieldRequest::BeginEdit { revision, .. } => {
                ime_state.transaction_changes = Vec::new();
                ime_state.transaction_revision = Some(revision);
                return Ok(());
            }
            txt::TextFieldRequest::CommitEdit { responder, .. } => {
                if ime_state.transaction_revision != Some(ime_state.revision) {
                    return responder.send(txt::Error::BadRevision);
                }
                let res = if ime_state.apply_transaction() {
                    ime_state.increment_revision(None, true);
                    responder.send(txt::Error::Ok)
                } else {
                    responder.send(txt::Error::BadRequest)
                };
                ime_state.transaction_changes = Vec::new();
                ime_state.transaction_revision = None;
                return res;
            }
            txt::TextFieldRequest::AbortEdit { .. } => {
                ime_state.transaction_changes = Vec::new();
                ime_state.transaction_revision = None;
                return Ok(());
            }
            req @ txt::TextFieldRequest::Replace { .. }
            | req @ txt::TextFieldRequest::SetSelection { .. }
            | req @ txt::TextFieldRequest::SetComposition { .. }
            | req @ txt::TextFieldRequest::ClearComposition { .. }
            | req @ txt::TextFieldRequest::SetDeadKeyHighlight { .. }
            | req @ txt::TextFieldRequest::ClearDeadKeyHighlight { .. } => {
                if ime_state.transaction_revision.is_some() {
                    ime_state.transaction_changes.push(req)
                }
                return Ok(());
            }
        }
    }

    /// Handles a request from the legancy IME API, an InputMethodEditorRequest.
    fn handle_ime_message(&self, msg: uii::InputMethodEditorRequest) {
        match msg {
            ImeReq::SetKeyboardType { keyboard_type, .. } => {
                let mut state = self.0.lock();
                state.keyboard_type = keyboard_type;
            }
            ImeReq::SetState { state, .. } => {
                self.set_state(idx::text_state_codeunit_to_byte(state));
            }
            ImeReq::InjectInput { event, .. } => {
                self.inject_input(event);
            }
            ImeReq::Show { .. } => {
                // clone to ensure we only hold one lock at a time
                let ime_service = self.0.lock().ime_service.clone();
                ime_service.show_keyboard();
            }
            ImeReq::Hide { .. } => {
                // clone to ensure we only hold one lock at a time
                let ime_service = self.0.lock().ime_service.clone();
                ime_service.hide_keyboard();
            }
        }
    }

    /// Sets the internal state. Expects input_state to use codeunits; automatically
    /// converts to byte indices before storing.
    pub fn set_state(&self, input_state: uii::TextInputState) {
        let mut state = self.0.lock();
        state.text_state = idx::text_state_codeunit_to_byte(input_state);
        // the old C++ IME implementation didn't call did_update_state here, so this second argument is false.
        state.increment_revision(None, false);
    }

    pub fn inject_input(&self, event: uii::InputEvent) {
        let mut state = self.0.lock();
        let keyboard_event = match event {
            uii::InputEvent::Keyboard(e) => e,
            _ => return,
        };

        if keyboard_event.phase == uii::KeyboardEventPhase::Pressed
            || keyboard_event.phase == uii::KeyboardEventPhase::Repeat
        {
            if keyboard_event.code_point != 0 {
                state.type_keycode(keyboard_event.code_point);
                state.increment_revision(Some(keyboard_event), true)
            } else {
                match keyboard_event.hid_usage {
                    HID_USAGE_KEY_BACKSPACE => {
                        state.delete_backward();
                        state.increment_revision(Some(keyboard_event), true);
                    }
                    HID_USAGE_KEY_DELETE => {
                        state.delete_forward();
                        state.increment_revision(Some(keyboard_event), true);
                    }
                    HID_USAGE_KEY_LEFT => {
                        state.cursor_horizontal_move(keyboard_event.modifiers, false);
                        state.increment_revision(Some(keyboard_event), true);
                    }
                    HID_USAGE_KEY_RIGHT => {
                        state.cursor_horizontal_move(keyboard_event.modifiers, true);
                        state.increment_revision(Some(keyboard_event), true);
                    }
                    HID_USAGE_KEY_ENTER => {
                        state.client.on_action(state.action).unwrap_or_else(|e| {
                            fx_log_warn!("error sending action to ImeClient: {:?}", e)
                        });
                    }
                    _ => {
                        // Not an editing key, forward the event to clients.
                        state.increment_revision(Some(keyboard_event), true);
                    }
                }
            }
        }
    }
}
