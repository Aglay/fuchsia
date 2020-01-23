// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::key_util::get_input_sequence_for_key_event,
    crate::pty::Pty,
    crate::ui::TerminalScene,
    anyhow::{Context as _, Error},
    carnelian::{
        make_message, AnimationMode, AppContext, Message, Size, ViewAssistant,
        ViewAssistantContext, ViewKey, ViewMessages,
    },
    fidl_fuchsia_hardware_pty::WindowSize,
    fidl_fuchsia_ui_input::KeyboardEvent,
    fuchsia_async as fasync, fuchsia_trace as ftrace,
    futures::{channel::mpsc, io::AsyncReadExt, select, FutureExt, StreamExt},
    std::{cell::RefCell, ffi::CStr, fs::File, io::prelude::*, rc::Rc},
    term_model::{
        ansi::Processor,
        config::Config,
        term::{SizeInfo, Term},
    },
};

#[cfg(test)]
use cstr::cstr;

const BYTE_BUFFER_MAX_SIZE: usize = 128;

struct ResizeEvent {
    window_size: WindowSize,
}

#[derive(Clone)]
struct AppContextWrapper {
    app_context: Option<AppContext>,
    test_sender: Option<mpsc::UnboundedSender<Message>>,
}

impl AppContextWrapper {
    fn queue_message(&self, target: ViewKey, message: Message) {
        // In a real environment we send to the app_context but we can optionally
        // send to the test_sender for observing what is sent to the framework.
        // This is needed because the framework does not give us access to observe
        // messages that are received. The if/else ensures that we do not send to
        // both places as this will fail the borrow check.
        if let Some(app_context) = &self.app_context {
            app_context.queue_message(target, message);
        } else if let Some(sender) = &self.test_sender {
            sender.unbounded_send(message).expect("Unable queue message to test_sender");
        }
    }

    #[cfg(test)]
    // Allows tests to observe what is sent to the app_context.
    fn use_test_sender(&mut self, sender: mpsc::UnboundedSender<Message>) {
        self.app_context = None;
        self.test_sender = Some(sender);
    }
}

struct PtyContext {
    resize_sender: mpsc::UnboundedSender<ResizeEvent>,
    file: File,
    resize_receiver: Option<mpsc::UnboundedReceiver<ResizeEvent>>,
    test_buffer: Option<Vec<u8>>,
}

impl PtyContext {
    fn from_pty(pty: &Pty) -> Result<PtyContext, Error> {
        let (resize_sender, resize_receiver) = mpsc::unbounded();
        let file = pty.try_clone_fd()?;
        Ok(PtyContext {
            resize_sender,
            file,
            resize_receiver: Some(resize_receiver),
            test_buffer: None,
        })
    }

    fn take_resize_receiver(&mut self) -> mpsc::UnboundedReceiver<ResizeEvent> {
        self.resize_receiver.take().expect("attempting to take resize receiver")
    }

    #[cfg(test)]
    fn allow_dual_write_for_test(&mut self) {
        self.test_buffer = Some(vec![]);
    }
}

impl Write for PtyContext {
    fn write(&mut self, buf: &[u8]) -> Result<usize, std::io::Error> {
        if let Some(test_buffer) = self.test_buffer.as_mut() {
            for b in buf {
                test_buffer.push(*b);
            }
        }
        self.file.write(buf)
    }

    fn flush(&mut self) -> Result<(), std::io::Error> {
        self.file.flush()
    }
}

pub struct TerminalViewAssistant {
    last_known_size: Size,
    pty_context: Option<PtyContext>,
    terminal_scene: TerminalScene,
    term: Rc<RefCell<Term>>,
    app_context: AppContextWrapper,
    view_key: ViewKey,

    /// If set, will use this command when spawning the pty, this is useful for tests.
    spawn_command: Option<&'static CStr>,
}

impl TerminalViewAssistant {
    /// Creates a new instance of the TerminalViewAssistant.
    pub fn new(app_context: &AppContext, view_key: ViewKey) -> TerminalViewAssistant {
        let cell_size = Size::new(12.0, 22.0);
        let term = Term::new(
            &Config::default(),
            SizeInfo {
                // set the initial size/width to be that of the cell size which prevents
                // the term from panicing if a byte is received before a resize event.
                width: cell_size.width,
                height: cell_size.height,
                cell_width: cell_size.width,
                cell_height: cell_size.height,
                padding_x: 0.0,
                padding_y: 0.0,
            },
        );

        TerminalViewAssistant {
            last_known_size: Size::zero(),
            pty_context: None,
            term: Rc::new(RefCell::new(term)),
            terminal_scene: TerminalScene::default(),
            app_context: AppContextWrapper {
                app_context: Some(app_context.clone()),
                test_sender: None,
            },
            view_key,
            spawn_command: None,
        }
    }

    #[cfg(test)]
    pub fn new_for_test() -> TerminalViewAssistant {
        let app_context = AppContext::new_for_testing_purposes_only();
        let mut view = Self::new(&app_context, 1);
        view.spawn_command = Some(cstr!("/pkg/bin/sh"));
        view
    }

    /// Checks if we need to perform a resize based on a new size.
    /// This method rounds pixels down to the next pixel value.
    fn needs_resize(prev_size: &Size, new_size: &Size) -> bool {
        prev_size.floor().not_equal(&new_size.floor()).any()
    }

    /// Checks to see if the size of terminal has changed and resizes if it has.
    fn resize_if_needed(&mut self, new_size: &Size, logical_size: &Size) -> Result<(), Error> {
        // The shell works on logical size units but the views operate based on the size
        if TerminalViewAssistant::needs_resize(&self.last_known_size, new_size) {
            let floored_size = new_size.floor();
            let term_size = TerminalScene::calculate_term_size_from_size(&floored_size);

            // we can safely call borrow_mut here because we are running the terminal
            // in single threaded mode. If we do move to a multithreaded model we will
            // get a compiler error since we are using spawn_local in our pty_loop.
            let mut term = self.term.borrow_mut();
            let last_size_info = term.size_info().clone();

            let cell_width = last_size_info.cell_width;
            let cell_height = last_size_info.cell_height;
            let padding_x = last_size_info.padding_x;
            let padding_y = last_size_info.padding_y;

            let term_size_info = SizeInfo {
                width: term_size.width,
                height: term_size.height,
                cell_width,
                cell_height,
                padding_x,
                padding_y,
            };

            term.resize(&term_size_info);
            drop(term);

            let window_size =
                WindowSize { width: logical_size.width as u32, height: logical_size.height as u32 };

            self.queue_resize_event(ResizeEvent { window_size })
                .context("unable to queue outgoing pty message")?;

            self.last_known_size = floored_size;
            self.terminal_scene.update_size(floored_size);
            self.terminal_scene.update_cell_size(Size::new(cell_width, cell_height));
        }
        Ok(())
    }

    /// Checks to see if the Pty has been spawned and if not it does so.
    fn spawn_pty_loop(&mut self) -> Result<(), Error> {
        if self.pty_context.is_some() {
            return Ok(());
        }

        let mut pty = Pty::new()?;
        let mut pty_context = PtyContext::from_pty(&pty)?;
        let mut resize_receiver = pty_context.take_resize_receiver();

        let app_context = self.app_context.clone();
        let view_key = self.view_key;

        let term_clone = self.term.clone();
        let spawn_command = self.spawn_command.clone();

        // We want spawn_local here to enforce the single threaded model. If we
        // do move to multithreaded we will need to refactor the term parsing
        // logic to account for thread safaty.
        fasync::spawn_local(async move {
            pty.spawn(spawn_command).await.expect("unable to spawn pty");

            let fd = pty.try_clone_fd().expect("unable to clone pty read fd");
            let mut evented_fd = unsafe {
                // EventedFd::new() is unsafe because it can't guarantee the lifetime of
                // the file descriptor passed to it exceeds the lifetime of the EventedFd.
                // Since we're cloning the file when passing it in, the EventedFd
                // effectively owns that file descriptor and thus controls it's lifetime.
                fasync::net::EventedFd::new(fd).expect("failed to create evented_fd for io_loop")
            };

            let mut write_fd = pty.try_clone_fd().expect("unable to clone pty write fd");
            let mut parser = Processor::new();

            let mut read_buf = [0u8; BYTE_BUFFER_MAX_SIZE];
            loop {
                let mut read_fut = evented_fd.read(&mut read_buf).fuse();
                select!(
                    result = read_fut => {
                        let read_count = result.unwrap_or_else(|e: std::io::Error| {
                            eprintln!(
                                "failed to read bytes from io_loop, dropping current message: {:?}",
                                e
                            );
                            0
                        });
                        ftrace::duration!("terminal", "parse_bytes", "len" => read_count as u32);
                        let mut term = term_clone.borrow_mut();
                        if read_count > 0 {
                            for byte in &read_buf[0..read_count] {
                                parser.advance(&mut *term, *byte, &mut write_fd);
                            }
                            app_context.queue_message(view_key, make_message(ViewMessages::Update));
                        }
                    },
                result = resize_receiver.next().fuse() => {
                    if let Some(event) = result {
                        pty.resize(event.window_size).await.unwrap_or_else(|e: anyhow::Error| {
                            eprintln!("failed to send resize message to pty: {:?}", e)
                        });
                        app_context.queue_message(view_key, make_message(ViewMessages::Update));
                    }
                }
                );
            }
        });

        self.pty_context = Some(pty_context);

        Ok(())
    }

    fn queue_resize_event(&mut self, event: ResizeEvent) -> Result<(), Error> {
        if let Some(pty_context) = &mut self.pty_context {
            pty_context.resize_sender.unbounded_send(event).context("Unable send resize event")?;
        }

        Ok(())
    }

    // This method is overloaded from the ViewAssistant trait so we can test the method.
    // The ViewAssistant trait requires a ViewAssistantContext which we do not use and
    // we cannot make. This allows us to call the method directly in the tests.
    fn handle_keyboard_event(&mut self, event: &KeyboardEvent) -> Result<(), Error> {
        if let Some(string) = get_input_sequence_for_key_event(event) {
            // In practice these writes will contain a small amount of data
            // so we can use a synchronous write. If that proves to not be the
            // case we will need to refactor to have buffered writing.
            if let Some(pty_context) = &mut self.pty_context {
                pty_context
                    .write_all(string.as_bytes())
                    .unwrap_or_else(|e| println!("failed to write character to pty: {}", e));
            }
        }

        Ok(())
    }
}

impl ViewAssistant for TerminalViewAssistant {
    fn setup(&mut self, _context: &ViewAssistantContext<'_>) -> Result<(), Error> {
        Ok(())
    }

    fn update(&mut self, context: &ViewAssistantContext<'_>) -> Result<(), Error> {
        ftrace::duration!("terminal", "TerminalViewAssistant:update");

        // we need to call spawn in this update block because calling it in the
        // setup method causes us to receive write events before the view is
        // prepared to draw.
        self.spawn_pty_loop()?;
        self.resize_if_needed(&context.size, &context.logical_size)?;

        // Tell the termnial scene to render the values
        let canvas = &mut context.canvas.as_ref().unwrap().borrow_mut();
        let config = Config::default();
        let term = self.term.borrow();

        let iter = {
            ftrace::duration!("terminal", "TerminalViewAssistant:update:renderable_cells");
            term.renderable_cells(&config, None /* selection */, true /* focused */)
        };

        self.terminal_scene.render(canvas, iter);
        Ok(())
    }

    fn handle_keyboard_event(
        &mut self,
        _: &mut ViewAssistantContext<'_>,
        event: &KeyboardEvent,
    ) -> Result<(), Error> {
        self.handle_keyboard_event(event)
    }

    fn initial_animation_mode(&mut self) -> AnimationMode {
        AnimationMode::None
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::anyhow,
        fidl_fuchsia_ui_input::KeyboardEventPhase,
        fuchsia_async::{DurationExt, Timer},
        fuchsia_zircon::DurationNum,
        futures::future::Either,
    };

    #[test]
    fn can_create_view() {
        let _ = TerminalViewAssistant::new_for_test();
    }

    #[test]
    fn needs_resize_false_for_zero_sizes() {
        let zero = Size::zero();
        assert_eq!(TerminalViewAssistant::needs_resize(&zero, &zero), false);
    }

    #[test]
    fn needs_resize_true_for_different_sizes() {
        let prev_size = Size::zero();
        let new_size = Size::new(100.0, 100.0);
        assert!(TerminalViewAssistant::needs_resize(&prev_size, &new_size));
    }

    #[test]
    fn needs_resize_true_different_width_same_height() {
        let prev_size = Size::new(100.0, 10.0);
        let new_size = Size::new(100.0, 100.0);
        assert!(TerminalViewAssistant::needs_resize(&prev_size, &new_size));
    }

    #[test]
    fn needs_resize_true_different_height_same_width() {
        let prev_size = Size::new(10.0, 100.0);
        let new_size = Size::new(100.0, 100.0);
        assert!(TerminalViewAssistant::needs_resize(&prev_size, &new_size));
    }

    #[test]
    fn needs_resize_false_when_rounding_down() {
        let prev_size = Size::new(100.0, 100.0);
        let new_size = Size::new(100.1, 100.0);
        assert_eq!(TerminalViewAssistant::needs_resize(&prev_size, &new_size), false);
    }

    #[test]
    fn term_is_resized_when_needed() {
        let mut view = TerminalViewAssistant::new_for_test();
        let new_size = Size::new(100.5, 100.9);
        view.resize_if_needed(&new_size, &Size::zero()).expect("call to resize failed");

        let term = view.term.borrow();

        let size_info = term.size_info();
        let expected_size = TerminalScene::calculate_term_size_from_size(&view.last_known_size);

        // we want to make sure that the values are floored and that they
        // match what the scene will render the terminal as.
        assert_eq!(size_info.width, expected_size.width);
        assert_eq!(size_info.height, expected_size.height);
    }

    #[test]
    fn last_known_size_is_floored_on_resize() {
        let mut view = TerminalViewAssistant::new_for_test();
        let new_size = Size::new(100.3, 100.4);
        view.resize_if_needed(&new_size, &Size::zero()).expect("call to resize failed");

        assert_eq!(view.last_known_size.width, 100.0);
        assert_eq!(view.last_known_size.height, 100.0);
    }

    #[fasync::run_singlethreaded(test)]
    async fn resize_message_queued_with_logical_size_when_resize_needed() -> Result<(), Error> {
        let pty = Pty::new()?;
        let mut pty_context = PtyContext::from_pty(&pty)?;
        let mut view = TerminalViewAssistant::new_for_test();
        let mut receiver = pty_context.take_resize_receiver();

        view.pty_context = Some(pty_context);

        view.resize_if_needed(&Size::new(100.0, 100.0), &Size::new(1000.0, 2000.0))
            .expect("call to resize failed");

        let event = receiver.next().await.expect("failed to receive pty event");
        assert_eq!(event.window_size.width, 1000);
        assert_eq!(event.window_size.height, 2000);

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn handle_keyboard_event_writes_characters() -> Result<(), Error> {
        let pty = Pty::new()?;
        let mut pty_context = PtyContext::from_pty(&pty)?;
        let mut view = TerminalViewAssistant::new_for_test();
        pty_context.allow_dual_write_for_test();

        view.pty_context = Some(pty_context);

        let capital_a = 65;
        view.handle_keyboard_event(&make_keyboard_event(capital_a))?;

        let test_buffer = view.pty_context.as_mut().unwrap().test_buffer.take().unwrap();
        assert_eq!(test_buffer, b"A");

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn pty_is_spawned_on_first_request() -> Result<(), Error> {
        let mut view = TerminalViewAssistant::new_for_test();
        view.spawn_pty_loop()?;
        assert!(view.pty_context.is_some());

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn pty_message_reads_trigger_call_to_redraw() -> Result<(), Error> {
        let (view, mut receiver) = make_test_view_with_spawned_pty_loop().await?;

        let mut fd = view
            .pty_context
            .as_ref()
            .map(|ctx| ctx.file.try_clone().expect("attempt to clone fd failed"))
            .unwrap();

        fasync::spawn_local(async move {
            let _ = fd.write_all(b"ls");
        });

        // No redraw will trigger a timeout and failure
        wait_until_update_received_or_timeout(&mut receiver)
            .await
            .context(":pty_message_reads_trigger_call_to_redraw after write")?;

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn resize_message_triggers_call_to_redraw() -> Result<(), Error> {
        let (mut view, mut receiver) = make_test_view_with_spawned_pty_loop().await?;

        let window_size = WindowSize { width: 123, height: 123 };

        view.queue_resize_event(ResizeEvent { window_size })
            .context("unable to queue outgoing pty message")?;

        // No redraw will trigger a timeout and failure
        wait_until_update_received_or_timeout(&mut receiver)
            .await
            .context(":resize_message_triggers_call_to_redraw after queue event")?;

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn bytes_written_are_processed_by_term() -> Result<(), Error> {
        let (mut view, mut receiver) = make_test_view_with_spawned_pty_loop().await?;

        // make sure we have a big enough size that a single character does not wrap
        let large_size = Size::new(1000.0, 1000.0);
        view.resize_if_needed(&large_size, &large_size)?;

        // Resizing will cause an update so we need to wait for that before we write.
        wait_until_update_received_or_timeout(&mut receiver)
            .await
            .context(":bytes_written_are_processed_by_term after resize_if_needed")?;

        let term = view.term.borrow();

        let col_pos_before = term.cursor().point.col;
        drop(term);

        let mut fd = view
            .pty_context
            .as_ref()
            .map(|ctx| ctx.file.try_clone().expect("attempt to clone fd failed"))
            .unwrap();

        fasync::spawn_local(async move {
            let _ = fd.write_all(b"A");
        });

        // Wait until we get a notice that the view is ready to redraw
        wait_until_update_received_or_timeout(&mut receiver)
            .await
            .context(":bytes_written_are_processed_by_term after write")?;

        let term = view.term.borrow();
        let col_pos_after = term.cursor().point.col;
        assert_eq!(col_pos_before + 1, col_pos_after);

        Ok(())
    }

    async fn make_test_view_with_spawned_pty_loop(
    ) -> Result<(TerminalViewAssistant, mpsc::UnboundedReceiver<Message>), Error> {
        let (sender, mut receiver) = mpsc::unbounded();

        let mut view = TerminalViewAssistant::new_for_test();
        view.app_context.use_test_sender(sender);

        let _ = view.spawn_pty_loop();

        // Spawning the loop triggers a read and a redraw, we want to skip this
        // so that we can check that our test event triggers the redraw.
        wait_until_update_received_or_timeout(&mut receiver)
            .await
            .context("::make_test_view_with_spawned_pty_loop")?;

        Ok((view, receiver))
    }

    async fn wait_until_update_received_or_timeout(
        receiver: &mut mpsc::UnboundedReceiver<Message>,
    ) -> Result<(), Error> {
        loop {
            let timeout = Timer::new(5000_i64.millis().after_now());
            let either = futures::future::select(timeout, receiver.next().fuse());
            let resolved = either.await;
            match resolved {
                Either::Left(_) => {
                    return Err(anyhow!("wait_until_update_received timed out"));
                }
                Either::Right((result, _)) => {
                    let message = result.expect("result should not be None");
                    if let Some(view_msg) = message.downcast_ref::<ViewMessages>() {
                        match view_msg {
                            ViewMessages::Update => break,
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn make_keyboard_event(code_point: u32) -> KeyboardEvent {
        KeyboardEvent {
            code_point: code_point,
            phase: KeyboardEventPhase::Pressed,
            device_id: 0 as u32,
            event_time: 0 as u64,
            hid_usage: 0 as u32,
            modifiers: 0 as u32,
        }
    }
}
