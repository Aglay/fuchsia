// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::input_device::{self, InputDeviceBinding, InputEvent},
    anyhow::{format_err, Error},
    async_trait::async_trait,
    fidl_fuchsia_input_report as fidl,
    fidl_fuchsia_input_report::{InputDeviceProxy, InputReport},
    fidl_fuchsia_ui_input as fidl_ui_input,
    fuchsia_syslog::fx_log_err,
    futures::channel::mpsc::Sender,
    maplit::hashmap,
    std::collections::HashMap,
    std::iter::FromIterator,
};

/// A [`TouchEvent`] represents a set of contacts and the phase those contacts are in.
///
/// For example, when a user touches a touch screen with two fingers, there will be two
/// [`TouchContact`]s. When a user removes one finger, there will still be two contacts
/// but one will be reported as removed.
///
/// The expected sequence for any given contact is:
/// 1. [`fidl_fuchsia_ui_input::PointerEventPhase::Add`]
/// 2. [`fidl_fuchsia_ui_input::PointerEventPhase::Down`]
/// 3. 0 or more [`fidl_fuchsia_ui_input::PointerEventPhase::Move`]
/// 4. [`fidl_fuchsia_ui_input::PointerEventPhase::Up`]
/// 5. [`fidl_fuchsia_ui_input::PointerEventPhase::Remove`]
///
/// Additionally, a [`fidl_fuchsia_ui_input::PointerEventPhase::Cancel`] may be sent at any time
/// signalling that the event is no longer directed towards the receiver.
#[derive(Clone, Debug, PartialEq)]
pub struct TouchEvent {
    /// The contacts associated with the touch event. For example, a two-finger touch would result
    /// in one touch event with two [`TouchContact`]s.
    ///
    /// Contacts are grouped based on their current phase (e.g., down, move).
    pub contacts: HashMap<fidl_ui_input::PointerEventPhase, Vec<TouchContact>>,
}

/// A [`TouchContact`] represents a single contact (e.g., one touch of a multi-touch gesture) related
/// to a touch event.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TouchContact {
    /// The identifier of the contact. Unique per touch device.
    pub id: u32,

    /// The x position of the touch event, in the units of the associated
    /// [`ContactDeviceDescriptor`]'s `x_range`.
    pub position_x: i64,

    /// The y position of the touch event, in the units of the associated
    /// [`ContactDeviceDescriptor`]'s `y_range`.
    pub position_y: i64,

    /// The pressure associated with the contact, in the units of the associated
    /// [`ContactDeviceDescriptor`]'s `pressure_range`.
    pub pressure: Option<i64>,

    /// The width of the touch event, in the units of the associated
    /// [`ContactDeviceDescriptor`]'s `width_range`.
    pub contact_width: Option<i64>,

    /// The height of the touch event, in the units of the associated
    /// [`ContactDeviceDescriptor`]'s `height_range`.
    pub contact_height: Option<i64>,
}

impl Eq for TouchContact {}

impl From<&fidl_fuchsia_input_report::ContactInputReport> for TouchContact {
    fn from(fidl_contact: &fidl_fuchsia_input_report::ContactInputReport) -> TouchContact {
        TouchContact {
            id: fidl_contact.contact_id.unwrap_or_default(),
            position_x: fidl_contact.position_x.unwrap_or_default(),
            position_y: fidl_contact.position_y.unwrap_or_default(),
            pressure: fidl_contact.pressure,
            contact_width: fidl_contact.contact_width,
            contact_height: fidl_contact.contact_height,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct TouchDeviceDescriptor {
    /// The id of the connected touch input device.
    pub device_id: u32,

    /// The descriptors for the possible contacts associated with the device.
    pub contacts: Vec<ContactDeviceDescriptor>,
}

/// A [`ContactDeviceDescriptor`] describes the possible values touch contact properties can take on.
///
/// This descriptor can be used, for example, to determine where on a screen a touch made contact.
///
/// # Example
///
/// ```
/// // Determine the scaling factor between the display and the touch device's x range.
/// let scaling_factor =
///     display_width / (contact_descriptor._x_range.end - contact_descriptor._x_range.start);
/// // Use the scaling factor to scale the contact report's x position.
/// let hit_location = scaling_factor * contact_report.position_x;
/// ```
#[derive(Clone, Debug, PartialEq)]
pub struct ContactDeviceDescriptor {
    /// The range of possible x values for this touch contact.
    pub x_range: fidl::Range,

    /// The range of possible y values for this touch contact.
    pub y_range: fidl::Range,

    /// The range of possible pressure values for this touch contact.
    pub pressure_range: Option<fidl::Range>,

    /// The range of possible widths for this touch contact.
    pub width_range: Option<fidl::Range>,

    /// The range of possible heights for this touch contact.
    pub height_range: Option<fidl::Range>,
}

/// A [`TouchBinding`] represents a connection to a touch input device.
///
/// The [`TouchBinding`] parses and exposes touch descriptor properties (e.g., the range of
/// possible x values for touch contacts) for the device it is associated with.
/// It also parses [`InputReport`]s from the device, and sends them to clients
/// via [`TouchBinding::input_event_stream()`].
///
/// # Example
/// ```
/// let mut touch_device: TouchBinding = input_device::InputDeviceBinding::new().await?;
///
/// while let Some(report) = touch_device.input_event_stream().next().await {}
/// ```
pub struct TouchBinding {
    /// The channel to stream InputEvents to.
    event_sender: Sender<InputEvent>,

    /// Holds information about this device.
    device_descriptor: TouchDeviceDescriptor,
}

#[async_trait]
impl input_device::InputDeviceBinding for TouchBinding {
    fn input_event_sender(&self) -> Sender<InputEvent> {
        self.event_sender.clone()
    }

    fn get_device_descriptor(&self) -> input_device::InputDeviceDescriptor {
        input_device::InputDeviceDescriptor::Touch(self.device_descriptor.clone())
    }
}

impl TouchBinding {
    /// Creates a new [`InputDeviceBinding`] from the `device_proxy`.
    ///
    /// The binding will start listening for input reports immediately and send new InputEvents
    /// to the InputPipeline over `input_event_sender`.
    ///
    /// # Parameters
    /// - `device_proxy`: The proxy to bind the new [`InputDeviceBinding`] to.
    /// - `input_event_sender`: The channel to send new InputEvents to.
    ///
    /// # Errors
    /// If there was an error binding to the proxy.
    pub async fn new(
        device_proxy: InputDeviceProxy,
        input_event_sender: Sender<input_device::InputEvent>,
    ) -> Result<Self, Error> {
        let device_binding = Self::bind_device(&device_proxy, input_event_sender).await?;
        input_device::initialize_report_stream(
            device_proxy,
            device_binding.get_device_descriptor(),
            device_binding.input_event_sender(),
            Self::process_reports,
        );

        Ok(device_binding)
    }

    /// Binds the provided input device to a new instance of `Self`.
    ///
    /// # Parameters
    /// - `device`: The device to use to initalize the binding.
    /// - `input_event_sender`: The channel to send new InputEvents to.
    ///
    /// # Errors
    /// If the device descriptor could not be retrieved, or the descriptor could
    /// not be parsed correctly.
    async fn bind_device(
        device: &InputDeviceProxy,
        input_event_sender: Sender<input_device::InputEvent>,
    ) -> Result<Self, Error> {
        let device_descriptor: fidl_fuchsia_input_report::DeviceDescriptor =
            device.get_descriptor().await?;

        match device_descriptor.touch {
            Some(fidl_fuchsia_input_report::TouchDescriptor {
                input:
                    Some(fidl_fuchsia_input_report::TouchInputDescriptor {
                        contacts: Some(contact_descriptors),
                        max_contacts: _,
                        touch_type: _,
                        buttons: _,
                    }),
            }) => Ok(TouchBinding {
                event_sender: input_event_sender,
                device_descriptor: TouchDeviceDescriptor {
                    device_id: 0,
                    contacts: contact_descriptors
                        .iter()
                        .map(TouchBinding::parse_contact_descriptor)
                        .filter_map(Result::ok)
                        .collect(),
                },
            }),
            descriptor => Err(format_err!("Touch Descriptor failed to parse: \n {:?}", descriptor)),
        }
    }

    /// Parses an [`InputReport`] into one or more [`InputEvent`]s.
    ///
    /// The [`InputEvent`]s are sent to the device binding owner via [`sender`].
    ///
    /// # Parameters
    /// - `report`: The incoming [`InputReport`].
    /// - `previous_report`: The previous [`InputReport`] seen for the same device. This can be
    ///                    used to determine, for example, which keys are no longer present in
    ///                    a keyboard report to generate key released events. If `None`, no
    ///                    previous report was found.
    /// - `device_descriptor`: The descriptor for the input device generating the input reports.
    /// - `input_event_sender`: The sender for the device binding's input event stream.
    ///
    /// # Returns
    /// An [`InputReport`] which will be passed to the next call to [`process_reports`], as
    /// [`previous_report`]. If `None`, the next call's [`previous_report`] will be `None`.
    fn process_reports(
        report: InputReport,
        previous_report: Option<InputReport>,
        device_descriptor: &input_device::InputDeviceDescriptor,
        input_event_sender: &mut Sender<InputEvent>,
    ) -> Option<InputReport> {
        // Input devices can have multiple types so ensure `report` is a TouchInputReport.
        let touch_report: &fidl_fuchsia_input_report::TouchInputReport = match &report.touch {
            Some(touch) => touch,
            None => {
                return previous_report;
            }
        };

        let previous_contacts: HashMap<u32, TouchContact> = previous_report
            .as_ref()
            .and_then(|unwrapped_report| unwrapped_report.touch.as_ref())
            .map(touch_contacts_from_touch_report)
            .unwrap_or_default();
        let current_contacts: HashMap<u32, TouchContact> =
            touch_contacts_from_touch_report(touch_report);

        // Contacts which exist only in current.
        let added_contacts: Vec<TouchContact> = Vec::from_iter(
            current_contacts
                .values()
                .cloned()
                .filter(|contact| !previous_contacts.contains_key(&contact.id)),
        );
        // Contacts which exist in both previous and current.
        let moved_contacts: Vec<TouchContact> = Vec::from_iter(
            current_contacts
                .values()
                .cloned()
                .filter(|contact| previous_contacts.contains_key(&contact.id)),
        );
        // Contacts which exist only in previous.
        let removed_contacts: Vec<TouchContact> = Vec::from_iter(
            previous_contacts
                .values()
                .cloned()
                .filter(|contact| !current_contacts.contains_key(&contact.id)),
        );

        let event_time: input_device::EventTime =
            input_device::event_time_or_now(report.event_time);

        send_event(
            hashmap! {
                fidl_ui_input::PointerEventPhase::Add => added_contacts.clone(),
                fidl_ui_input::PointerEventPhase::Down => added_contacts,
                fidl_ui_input::PointerEventPhase::Move => moved_contacts,
                fidl_ui_input::PointerEventPhase::Up => removed_contacts.clone(),
                fidl_ui_input::PointerEventPhase::Remove => removed_contacts,
            },
            device_descriptor,
            event_time,
            input_event_sender,
        );

        Some(report)
    }

    /// Parses a FIDL contact descriptor into a [`ContactDeviceDescriptor`]
    ///
    /// # Parameters
    /// - `contact_device_descriptor`: The contact descriptor to parse.
    ///
    /// # Errors
    /// If the contact description fails to parse because required fields aren't present.
    fn parse_contact_descriptor(
        contact_device_descriptor: &fidl::ContactInputDescriptor,
    ) -> Result<ContactDeviceDescriptor, Error> {
        match contact_device_descriptor {
            fidl::ContactInputDescriptor {
                position_x: Some(x_axis),
                position_y: Some(y_axis),
                pressure: pressure_axis,
                contact_width: width_axis,
                contact_height: height_axis,
            } => Ok(ContactDeviceDescriptor {
                x_range: x_axis.range,
                y_range: y_axis.range,
                pressure_range: pressure_axis.map(|axis| axis.range),
                width_range: width_axis.map(|axis| axis.range),
                height_range: height_axis.map(|axis| axis.range),
            }),
            descriptor => {
                Err(format_err!("Touch Contact Descriptor failed to parse: \n {:?}", descriptor))
            }
        }
    }
}

fn touch_contacts_from_touch_report(
    touch_report: &fidl_fuchsia_input_report::TouchInputReport,
) -> HashMap<u32, TouchContact> {
    // First unwrap all the optionals in the input report to get to the contacts.
    let contacts: Vec<TouchContact> = touch_report
        .contacts
        .as_ref()
        .and_then(|unwrapped_contacts| {
            // Once the contacts are found, convert them into `TouchContact`s.
            Some(unwrapped_contacts.iter().map(TouchContact::from).collect())
        })
        .unwrap_or_default();

    contacts.into_iter().map(|contact| (contact.id, contact)).collect()
}

/// Sends a TouchEvent over `input_event_sender`.
///
/// # Parameters
/// - `contacts`: The contact points relevant to the new TouchEvent.
/// - `device_descriptor`: The descriptor for the input device generating the input reports.
/// - `event_time`: The time in nanoseconds when the event was first recorded.
/// - `input_event_sender`: The sender for the device binding's input event stream.
fn send_event(
    contacts: HashMap<fidl_ui_input::PointerEventPhase, Vec<TouchContact>>,
    device_descriptor: &input_device::InputDeviceDescriptor,
    event_time: input_device::EventTime,
    input_event_sender: &mut Sender<input_device::InputEvent>,
) {
    match input_event_sender.try_send(input_device::InputEvent {
        device_event: input_device::InputDeviceEvent::Touch(TouchEvent { contacts }),
        device_descriptor: device_descriptor.clone(),
        event_time,
    }) {
        Err(e) => fx_log_err!("Failed to send TouchEvent with error: {:?}", e),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::testing_utilities::{
            self, create_touch_contact, create_touch_event, create_touch_input_report,
        },
        fuchsia_async as fasync,
        futures::StreamExt,
    };

    // Tests that a input report with a new contact generates an event with an add and a down.
    #[fasync::run_singlethreaded(test)]
    async fn add_and_down() {
        const TOUCH_ID: u32 = 2;

        let descriptor = input_device::InputDeviceDescriptor::Touch(TouchDeviceDescriptor {
            device_id: 1,
            contacts: vec![],
        });
        let (event_time_i64, event_time_u64) = testing_utilities::event_times();

        let contact = fidl_fuchsia_input_report::ContactInputReport {
            contact_id: Some(TOUCH_ID),
            position_x: Some(0),
            position_y: Some(0),
            pressure: None,
            contact_width: None,
            contact_height: None,
        };
        let reports = vec![create_touch_input_report(vec![contact], event_time_i64)];

        let expected_events = vec![create_touch_event(
            hashmap! {
                fidl_ui_input::PointerEventPhase::Add
                    => vec![create_touch_contact(TOUCH_ID, 0, 0)],
                fidl_ui_input::PointerEventPhase::Down
                    => vec![create_touch_contact(TOUCH_ID, 0, 0)],
            },
            event_time_u64,
            &descriptor,
        )];

        assert_input_report_sequence_generates_events!(
            input_reports: reports,
            expected_events: expected_events,
            device_descriptor: descriptor,
            device_type: TouchBinding,
        );
    }

    // Tests that up and remove events are sent when a touch is released.
    #[fasync::run_singlethreaded(test)]
    async fn up_and_remove() {
        const TOUCH_ID: u32 = 2;

        let descriptor = input_device::InputDeviceDescriptor::Touch(TouchDeviceDescriptor {
            device_id: 1,
            contacts: vec![],
        });
        let (event_time_i64, event_time_u64) = testing_utilities::event_times();

        let contact = fidl_fuchsia_input_report::ContactInputReport {
            contact_id: Some(TOUCH_ID),
            position_x: Some(0),
            position_y: Some(0),
            pressure: None,
            contact_width: None,
            contact_height: None,
        };
        let reports = vec![
            create_touch_input_report(vec![contact], event_time_i64),
            create_touch_input_report(vec![], event_time_i64),
        ];

        let expected_events = vec![
            create_touch_event(
                hashmap! {
                    fidl_ui_input::PointerEventPhase::Add
                        => vec![create_touch_contact(TOUCH_ID, 0, 0)],
                    fidl_ui_input::PointerEventPhase::Down
                        => vec![create_touch_contact(TOUCH_ID, 0, 0)],
                },
                event_time_u64,
                &descriptor,
            ),
            create_touch_event(
                hashmap! {
                    fidl_ui_input::PointerEventPhase::Up
                        => vec![create_touch_contact(TOUCH_ID, 0, 0)],
                    fidl_ui_input::PointerEventPhase::Remove
                        => vec![create_touch_contact(TOUCH_ID, 0, 0)],
                },
                event_time_u64,
                &descriptor,
            ),
        ];

        assert_input_report_sequence_generates_events!(
            input_reports: reports,
            expected_events: expected_events,
            device_descriptor: descriptor,
            device_type: TouchBinding,
        );
    }

    // Tests that a move generates the correct event.
    #[fasync::run_singlethreaded(test)]
    async fn add_down_move() {
        const TOUCH_ID: u32 = 2;
        const FIRST_X: i64 = 10;
        const FIRST_Y: i64 = 30;

        let descriptor = input_device::InputDeviceDescriptor::Touch(TouchDeviceDescriptor {
            device_id: 1,
            contacts: vec![],
        });
        let (event_time_i64, event_time_u64) = testing_utilities::event_times();

        let first_contact = fidl_fuchsia_input_report::ContactInputReport {
            contact_id: Some(TOUCH_ID),
            position_x: Some(FIRST_X),
            position_y: Some(FIRST_Y),
            pressure: None,
            contact_width: None,
            contact_height: None,
        };
        let second_contact = fidl_fuchsia_input_report::ContactInputReport {
            contact_id: Some(TOUCH_ID),
            position_x: Some(FIRST_X * 2),
            position_y: Some(FIRST_Y * 2),
            pressure: None,
            contact_width: None,
            contact_height: None,
        };

        let reports = vec![
            create_touch_input_report(vec![first_contact], event_time_i64),
            create_touch_input_report(vec![second_contact], event_time_i64),
        ];

        let expected_events = vec![
            create_touch_event(
                hashmap! {
                    fidl_ui_input::PointerEventPhase::Add
                        => vec![create_touch_contact(TOUCH_ID, FIRST_X, FIRST_Y)],
                    fidl_ui_input::PointerEventPhase::Down
                        => vec![create_touch_contact(TOUCH_ID, FIRST_X, FIRST_Y)],
                },
                event_time_u64,
                &descriptor,
            ),
            create_touch_event(
                hashmap! {
                    fidl_ui_input::PointerEventPhase::Move
                        => vec![create_touch_contact(TOUCH_ID, FIRST_X*2, FIRST_Y*2)],
                },
                event_time_u64,
                &descriptor,
            ),
        ];

        assert_input_report_sequence_generates_events!(
            input_reports: reports,
            expected_events: expected_events,
            device_descriptor: descriptor,
            device_type: TouchBinding,
        );
    }
}
