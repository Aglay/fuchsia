// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{
    convert::TryFrom,
    thread,
    time::{Duration, SystemTime},
};

use anyhow::{ensure, Error};

use fidl::endpoints::{self, ServerEnd};

use fidl_fuchsia_ui_input::{
    self, Axis, AxisScale, DeviceDescriptor, InputDeviceMarker, InputDeviceProxy,
    InputDeviceRegistryMarker, KeyboardDescriptor, MediaButtonsDescriptor, Range, Touch,
    TouchscreenDescriptor,
};

use crate::{inverse_keymap::InverseKeymap, keymaps, legacy_backend::*, usages::Usages};
use fuchsia_component as app;

pub(crate) trait ServerConsumer {
    fn consume(
        &mut self,
        device: &mut DeviceDescriptor,
        server: ServerEnd<InputDeviceMarker>,
    ) -> Result<(), Error>;
}

pub(crate) struct RegistryServerConsumer;

impl ServerConsumer for RegistryServerConsumer {
    fn consume(
        &mut self,
        device: &mut DeviceDescriptor,
        server: ServerEnd<InputDeviceMarker>,
    ) -> Result<(), Error> {
        let registry = app::client::connect_to_service::<InputDeviceRegistryMarker>()?;
        registry.register_device(device, server)?;

        Ok(())
    }
}

macro_rules! register_device {
    ( $consumer:expr , $field:ident : $value:expr ) => {{
        let mut device = DeviceDescriptor {
            device_info: None,
            keyboard: None,
            media_buttons: None,
            mouse: None,
            stylus: None,
            touchscreen: None,
            sensor: None,
        };
        device.$field = Some(Box::new($value));

        let (input_device_client, input_device_server) =
            endpoints::create_endpoints::<InputDeviceMarker>()?;
        $consumer.consume(&mut device, input_device_server)?;

        Ok(input_device_client.into_proxy()?)
    }};
}

fn register_touchscreen(
    consumer: &mut dyn ServerConsumer,
    width: u32,
    height: u32,
) -> Result<InputDeviceProxy, Error> {
    register_device! {
        consumer,
        touchscreen: TouchscreenDescriptor {
            x: Axis {
                range: Range { min: 0, max: width as i32 },
                resolution: 1,
                scale: AxisScale::Linear,
            },
            y: Axis {
                range: Range { min: 0, max: height as i32 },
                resolution: 1,
                scale: AxisScale::Linear,
            },
            max_finger_id: 255,
        }
    }
}

fn register_keyboard(consumer: &mut dyn ServerConsumer) -> Result<InputDeviceProxy, Error> {
    register_device! {
        consumer,
        keyboard: KeyboardDescriptor {
            keys: (Usages::HidUsageKeyA as u32..Usages::HidUsageKeyRightGui as u32).collect(),
        }
    }
}

fn register_media_buttons(consumer: &mut dyn ServerConsumer) -> Result<InputDeviceProxy, Error> {
    register_device! {
        consumer,
        media_buttons: MediaButtonsDescriptor {
            buttons: fidl_fuchsia_ui_input::MIC_MUTE
                | fidl_fuchsia_ui_input::VOLUME_DOWN
                | fidl_fuchsia_ui_input::VOLUME_UP,
        }
    }
}

fn nanos_from_epoch() -> Result<u64, Error> {
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|duration| duration.as_nanos() as u64)
        .map_err(Into::into)
}

fn repeat_with_delay(
    times: usize,
    delay: Duration,
    f1: impl Fn(usize) -> Result<(), Error>,
    f2: impl Fn(usize) -> Result<(), Error>,
) -> Result<(), Error> {
    for i in 0..times {
        f1(i)?;
        thread::sleep(delay);
        f2(i)?;
    }

    Ok(())
}

pub(crate) fn media_button_event(
    volume_up: bool,
    volume_down: bool,
    mic_mute: bool,
    reset: bool,
    pause: bool,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_media_buttons(consumer)?;

    input_device
        .dispatch_report(&mut media_buttons(
            volume_up,
            volume_down,
            mic_mute,
            reset,
            pause,
            nanos_from_epoch()?,
        ))
        .map_err(Into::into)
}

pub(crate) fn keyboard_event(
    usage: u32,
    duration: Duration,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_keyboard(consumer)?;

    repeat_with_delay(
        1,
        duration,
        |_| {
            // Key pressed.
            input_device
                .dispatch_report(&mut key_press_usage(Some(usage), nanos_from_epoch()?))
                .map_err(Into::into)
        },
        |_| {
            // Key released.
            input_device
                .dispatch_report(&mut key_press_usage(None, nanos_from_epoch()?))
                .map_err(Into::into)
        },
    )
}

pub(crate) fn text(
    input: String,
    duration: Duration,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_keyboard(consumer)?;
    let key_sequence = InverseKeymap::new(keymaps::QWERTY_MAP)
        .derive_key_sequence(&input)
        .ok_or_else(|| anyhow::format_err!("Cannot translate text to key sequence"))?;

    let stroke_duration = duration / (key_sequence.len() - 1) as u32;
    let mut key_iter = key_sequence.into_iter().peekable();

    while let Some(keyboard) = key_iter.next() {
        let result: Result<(), Error> = input_device
            .dispatch_report(&mut key_press(keyboard, nanos_from_epoch()?))
            .map_err(Into::into);
        result?;

        if key_iter.peek().is_some() {
            thread::sleep(stroke_duration);
        }
    }

    Ok(())
}

pub(crate) fn tap_event(
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    tap_event_count: usize,
    duration: Duration,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_touchscreen(consumer, width, height)?;
    let tap_duration = duration / tap_event_count as u32;

    repeat_with_delay(
        tap_event_count,
        tap_duration,
        |_| {
            // Touch down.
            input_device
                .dispatch_report(&mut tap(Some((x, y)), nanos_from_epoch()?))
                .map_err(Into::into)
        },
        |_| {
            // Touch up.
            input_device.dispatch_report(&mut tap(None, nanos_from_epoch()?)).map_err(Into::into)
        },
    )
}

pub(crate) fn multi_finger_tap_event(
    fingers: Vec<Touch>,
    width: u32,
    height: u32,
    tap_event_count: usize,
    duration: Duration,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_touchscreen(consumer, width, height)?;
    let multi_finger_tap_duration = duration / tap_event_count as u32;

    repeat_with_delay(
        tap_event_count,
        multi_finger_tap_duration,
        |_| {
            // Touch down.
            input_device
                .dispatch_report(&mut multi_finger_tap(Some(fingers.clone()), nanos_from_epoch()?))
                .map_err(Into::into)
        },
        |_| {
            // Touch up.
            input_device
                .dispatch_report(&mut multi_finger_tap(None, nanos_from_epoch()?))
                .map_err(Into::into)
        },
    )
}

pub(crate) fn swipe(
    x0: u32,
    y0: u32,
    x1: u32,
    y1: u32,
    width: u32,
    height: u32,
    move_event_count: usize,
    duration: Duration,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    multi_finger_swipe(
        vec![(x0, y0)],
        vec![(x1, y1)],
        width,
        height,
        move_event_count,
        duration,
        consumer,
    )
}

pub(crate) fn multi_finger_swipe(
    start_fingers: Vec<(u32, u32)>,
    end_fingers: Vec<(u32, u32)>,
    width: u32,
    height: u32,
    move_event_count: usize,
    duration: Duration,
    consumer: &mut dyn ServerConsumer,
) -> Result<(), Error> {
    ensure!(
        start_fingers.len() == end_fingers.len(),
        "start_fingers.len() != end_fingers.len() ({} != {})",
        start_fingers.len(),
        end_fingers.len()
    );
    ensure!(
        u32::try_from(start_fingers.len() + 1).is_ok(),
        "fingers exceed capacity of `finger_id`!"
    );

    let input_device = register_touchscreen(consumer, width, height)?;

    // Note: coordinates are coverted to `f64` before subtraction, because u32 subtraction
    // would overflow when swiping from higher coordinates to lower coordinates.
    let finger_delta_x = start_fingers
        .iter()
        .zip(end_fingers.iter())
        .map(|((start_x, _start_y), (end_x, _end_y))| {
            (*end_x as f64 - *start_x as f64) / std::cmp::max(move_event_count, 1) as f64
        })
        .collect::<Vec<_>>();
    let finger_delta_y = start_fingers
        .iter()
        .zip(end_fingers.iter())
        .map(|((_start_x, start_y), (_end_x, end_y))| {
            (*end_y as f64 - *start_y as f64) / std::cmp::max(move_event_count, 1) as f64
        })
        .collect::<Vec<_>>();

    let swipe_event_delay = if move_event_count > 1 {
        // We have move_event_count + 2 events:
        //   DOWN
        //   MOVE x move_event_count
        //   UP
        // so we need (move_event_count + 1) delays.
        duration / (move_event_count + 1) as u32
    } else {
        duration
    };

    repeat_with_delay(
        move_event_count + 2, // +2 to account for DOWN and UP events
        swipe_event_delay,
        |i| {
            let time = nanos_from_epoch()?;
            let mut report = match i {
                // DOWN
                0 => multi_finger_tap(
                    Some(
                        start_fingers
                            .iter()
                            .enumerate()
                            .map(|(finger_index, (x, y))| Touch {
                                finger_id: (finger_index + 1) as u32,
                                x: *x as i32,
                                y: *y as i32,
                                width: 0,
                                height: 0,
                            })
                            .collect(),
                    ),
                    time,
                ),
                // MOVE
                i if i <= move_event_count => multi_finger_tap(
                    Some(
                        start_fingers
                            .iter()
                            .enumerate()
                            .map(|(finger_index, (x, y))| Touch {
                                finger_id: (finger_index + 1) as u32,
                                x: (*x as f64 + (i as f64 * finger_delta_x[finger_index]).round())
                                    as i32,
                                y: (*y as f64 + (i as f64 * finger_delta_y[finger_index]).round())
                                    as i32,
                                width: 0,
                                height: 0,
                            })
                            .collect(),
                    ),
                    time,
                ),
                // UP
                i if i == (move_event_count + 1) => multi_finger_tap(None, time),
                i => panic!("unexpected loop iteration {}", i),
            };

            input_device.dispatch_report(&mut report).map_err(Into::into)
        },
        |_| Ok(()),
    )
}

#[cfg(test)]
mod tests {
    use {super::*, fuchsia_async as fasync};

    mod event_synthesis {
        use {
            super::*,
            anyhow::Context,
            fidl_fuchsia_ui_input::{
                InputDeviceRequest, InputDeviceRequestStream, InputReport, KeyboardReport,
                MediaButtonsReport, TouchscreenReport,
            },
            futures::stream::StreamExt,
        };

        // Like `InputReport`, but with the `Box`-ed items inlined.
        struct InlineInputReport {
            keyboard: Option<KeyboardReport>,
            media_buttons: Option<MediaButtonsReport>,
            touchscreen: Option<TouchscreenReport>,
        }

        impl InlineInputReport {
            fn new(input_report: InputReport) -> Self {
                Self {
                    keyboard: input_report.keyboard.map(|boxed| *boxed),
                    media_buttons: input_report.media_buttons.map(|boxed| *boxed),
                    touchscreen: input_report.touchscreen.map(|boxed| *boxed),
                }
            }
        }
        struct TestConsumer {
            event_stream: Option<InputDeviceRequestStream>,
        }

        impl ServerConsumer for TestConsumer {
            fn consume(
                &mut self,
                _: &mut DeviceDescriptor,
                server: ServerEnd<InputDeviceMarker>,
            ) -> Result<(), Error> {
                self.event_stream =
                    Some(server.into_stream().context("converting server to stream")?);
                Ok(())
            }
        }

        impl TestConsumer {
            fn new() -> Self {
                Self { event_stream: None }
            }

            async fn get_events(self: Self) -> Vec<Result<InlineInputReport, String>> {
                match self.event_stream {
                    Some(event_stream) => {
                        event_stream
                            .map(|fidl_result| match fidl_result {
                                Ok(InputDeviceRequest::DispatchReport { report, .. }) => {
                                    Ok(InlineInputReport::new(report))
                                }
                                Err(fidl_error) => Err(format!("FIDL error: {}", fidl_error)),
                            })
                            .collect()
                            .await
                    }
                    None => {
                        vec![Err(format!("called get_events() on Consumer with no `event_stream`"))]
                    }
                }
            }
        }

        /// Transforms an `IntoIterator<Item = Result<InlineInputReport, _>>` into a
        /// `Vec<Result</* $field-specific-type */, _>>`, by projecting `$field` out of the
        /// `InlineInputReport`s.
        macro_rules! project {
            ( $events:expr, $field:ident ) => {
                $events
                    .into_iter()
                    .map(|result| result.map(|report| report.$field))
                    .collect::<Vec<_>>()
            };
        }

        #[fasync::run_singlethreaded(test)]
        async fn media_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            media_button_event(true, false, true, false, true, &mut fake_event_listener)?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, media_buttons),
                [Ok(Some(MediaButtonsReport {
                    volume_up: true,
                    volume_down: false,
                    mic_mute: true,
                    reset: false,
                    pause: true,
                }))]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn keyboard_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            keyboard_event(40, Duration::from_millis(0), &mut fake_event_listener)?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, keyboard),
                [
                    Ok(Some(KeyboardReport { pressed_keys: vec![40] })),
                    Ok(Some(KeyboardReport { pressed_keys: vec![] }))
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn text_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            text("A".to_string(), Duration::from_millis(0), &mut fake_event_listener)?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, keyboard),
                [
                    Ok(Some(KeyboardReport { pressed_keys: vec![225] })),
                    Ok(Some(KeyboardReport { pressed_keys: vec![4, 225] })),
                    Ok(Some(KeyboardReport { pressed_keys: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn multi_finger_tap_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            let fingers = vec![
                Touch { finger_id: 1, x: 0, y: 0, width: 0, height: 0 },
                Touch { finger_id: 2, x: 20, y: 20, width: 0, height: 0 },
                Touch { finger_id: 3, x: 40, y: 40, width: 0, height: 0 },
                Touch { finger_id: 4, x: 60, y: 60, width: 0, height: 0 },
            ];
            multi_finger_tap_event(
                fingers,
                1000,
                1000,
                1,
                Duration::from_millis(0),
                &mut fake_event_listener,
            )?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 0, y: 0, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 20, y: 20, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 40, y: 40, width: 0, height: 0 },
                            Touch { finger_id: 4, x: 60, y: 60, width: 0, height: 0 },
                        ],
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn tap_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            tap_event(10, 10, 1000, 1000, 1, Duration::from_millis(0), &mut fake_event_listener)?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 10, y: 10, width: 0, height: 0 }]
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn swipe_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            swipe(
                10,
                10,
                100,
                100,
                1000,
                1000,
                2,
                Duration::from_millis(0),
                &mut fake_event_listener,
            )?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 10, y: 10, width: 0, height: 0 }],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 55, y: 55, width: 0, height: 0 }],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 100, y: 100, width: 0, height: 0 }],
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn swipe_event_report_inverted() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            swipe(
                100,
                100,
                10,
                10,
                1000,
                1000,
                2,
                Duration::from_millis(0),
                &mut fake_event_listener,
            )?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 100, y: 100, width: 0, height: 0 }],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 55, y: 55, width: 0, height: 0 }],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![Touch { finger_id: 1, x: 10, y: 10, width: 0, height: 0 }],
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn multi_finger_swipe_event_report() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            multi_finger_swipe(
                vec![(10, 10), (20, 20), (30, 30)],
                vec![(100, 100), (120, 120), (150, 150)],
                1000,
                1000,
                2,
                Duration::from_millis(0),
                &mut fake_event_listener,
            )?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 10, y: 10, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 20, y: 20, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 30, y: 30, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 55, y: 55, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 70, y: 70, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 90, y: 90, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 100, y: 100, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 120, y: 120, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 150, y: 150, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn multi_finger_swipe_event_report_inverted() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            multi_finger_swipe(
                vec![(100, 100), (120, 120), (150, 150)],
                vec![(10, 10), (20, 20), (30, 30)],
                1000,
                1000,
                2,
                Duration::from_millis(0),
                &mut fake_event_listener,
            )?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 100, y: 100, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 120, y: 120, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 150, y: 150, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 55, y: 55, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 70, y: 70, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 90, y: 90, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 10, y: 10, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 20, y: 20, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 30, y: 30, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }

        #[fasync::run_singlethreaded(test)]
        async fn multi_finger_swipe_event_zero_move_events() -> Result<(), Error> {
            let mut fake_event_listener = TestConsumer::new();
            multi_finger_swipe(
                vec![(10, 10), (20, 20), (30, 30)],
                vec![(100, 100), (120, 120), (150, 150)],
                1000,
                1000,
                0,
                Duration::from_millis(0),
                &mut fake_event_listener,
            )?;
            assert_eq!(
                project!(fake_event_listener.get_events().await, touchscreen),
                [
                    Ok(Some(TouchscreenReport {
                        touches: vec![
                            Touch { finger_id: 1, x: 10, y: 10, width: 0, height: 0 },
                            Touch { finger_id: 2, x: 20, y: 20, width: 0, height: 0 },
                            Touch { finger_id: 3, x: 30, y: 30, width: 0, height: 0 }
                        ],
                    })),
                    Ok(Some(TouchscreenReport { touches: vec![] })),
                ]
            );
            Ok(())
        }
    }
}
