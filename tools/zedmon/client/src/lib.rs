// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::protocol::{self, ParameterValue, ReportFormat, Value, MAX_PACKET_SIZE},
    anyhow::{bail, format_err, Error},
    std::{
        cell::RefCell,
        collections::HashMap,
        io::{Read, Write},
        os::raw::{c_uchar, c_ushort},
        time::SystemTime,
    },
    usb_bulk::{InterfaceInfo, Open},
};

const GOOGLE_VENDOR_ID: c_ushort = 0x18d1;
const ZEDMON_PRODUCT_ID: c_ushort = 0xaf00;
const VENDOR_SPECIFIC_CLASS_ID: c_uchar = 0xff;
const ZEDMON_SUBCLASS_ID: c_uchar = 0xff;
const ZEDMON_PROTOCOL_ID: c_uchar = 0x00;

/// Matches the USB interface info of a Zedmon device.
fn zedmon_match(ifc: &InterfaceInfo) -> bool {
    (ifc.dev_vendor == GOOGLE_VENDOR_ID)
        && (ifc.dev_product == ZEDMON_PRODUCT_ID)
        && (ifc.ifc_class == VENDOR_SPECIFIC_CLASS_ID)
        && (ifc.ifc_subclass == ZEDMON_SUBCLASS_ID)
        && (ifc.ifc_protocol == ZEDMON_PROTOCOL_ID)
}

/// Used by ZedmonClient to determine when data reporting should stop.
pub trait StopSignal {
    fn should_stop(&mut self) -> Result<bool, Error>;
}

/// Writes a Report to a CSV-formatted line of output.
fn report_to_string(
    report: protocol::Report,
    shunt_resistance: f32,
    v_shunt_index: usize,
    v_bus_index: usize,
) -> String {
    let v_shunt = report.values[v_shunt_index];
    let v_bus = report.values[v_bus_index];
    let (v_shunt, v_bus) = match (v_shunt, v_bus) {
        (Value::F32(x), Value::F32(y)) => (x, y),
        t => panic!("Got wrong value types for (v_shunt, v_bus): {:?}", t),
    };

    let power = v_bus * v_shunt / shunt_resistance;
    format!("{},{},{},{}\n", report.timestamp_micros, v_shunt, v_bus, power)
}

/// Interface to a Zedmon device.
#[derive(Debug)]
pub struct ZedmonClient<InterfaceType>
where
    InterfaceType: usb_bulk::Open<InterfaceType> + Read + Write,
{
    /// USB interface to the Zedmon device, or equivalent.
    interface: RefCell<InterfaceType>,

    /// Format of each field in a Report.
    field_formats: Vec<ReportFormat>,

    /// Information necessary to obtain power data from direct Zedmon measurements.
    shunt_resistance: f32,
    v_shunt_index: usize,
    v_bus_index: usize,
}

impl<InterfaceType: usb_bulk::Open<InterfaceType> + Read + Write> ZedmonClient<InterfaceType> {
    /// Enumerates all connected Zedmons. Returns a `Vec<String>` of their serial numbers.
    fn enumerate() -> Vec<String> {
        let mut serials = Vec::new();

        // Instead of matching any devices, this callback extracts Zedmon serial numbers as
        // InterfaceType::open iterates through them. InterfaceType::open is expected to return an
        // error because no devices match.
        let mut cb = |info: &InterfaceInfo| -> bool {
            if zedmon_match(info) {
                let null_pos = match info.serial_number.iter().position(|&c| c == 0) {
                    Some(p) => p,
                    None => {
                        eprintln!("Warning: Detected a USB device whose serial number was not null-terminated:");
                        eprintln!(
                            "{}",
                            (*String::from_utf8_lossy(&info.serial_number)).to_string()
                        );
                        return false;
                    }
                };
                serials
                    .push((*String::from_utf8_lossy(&info.serial_number[..null_pos])).to_string());
            }
            false
        };

        assert!(
            InterfaceType::open(&mut cb).is_err(),
            "open() should return an error, as the supplied callback cannot match any devices."
        );
        serials
    }

    // Number of USB packets that can be queued in Zedmon's USB interface, based on STM32F072
    // hardware limitations. The firmware currently only enqueues one packet, but the STM32F072 does
    // support double-buffering.
    const ZEDMON_USB_QUEUE_SIZE: u32 = 2;

    /// Disables reporting and drains all enqueued packets from `interface`.
    ///
    /// This should be done if a packet of incorrect type is received on the first read from
    /// `interface`. Typically, that scenario occurs if a previous invocation of the client
    /// terminated irregularly and left reporting enabled, but in principle a packet could still be
    /// enqueued from another request as well.
    ///
    /// Empirically, this process takes 4-5x as long as the timeout configured on `interface`, and
    /// it should not be performed unconditionally to avoid unnecessary delays.
    ///
    /// An error is returned if a packet is still received once the packet queue should be clear.
    fn disable_reporting_and_drain_packets(interface: &mut InterfaceType) -> Result<(), Error> {
        Self::disable_reporting(interface)?;

        // Read packets from the USB interface until an error (assumed to be due to lack of packets
        // -- the reason for an error is not exposed) is encountered. If we do not encounter an
        // error after reading more than the queue length, Zedmon is in an unexpected state.
        let mut response = [0; MAX_PACKET_SIZE];
        for _ in 0..Self::ZEDMON_USB_QUEUE_SIZE + 1 {
            if interface.read(&mut response).is_err() {
                return Ok(());
            }
        }
        return Err(format_err!(
            "The Zedmon device is in an unexpected state; received more than {} packets after \
            disabling reporting. Consider rebooting the device.",
            Self::ZEDMON_USB_QUEUE_SIZE
        ));
    }

    /// Creates a new ZedmonClient instance.
    // TODO(fxbug.dev/61148): Make the behavior predictable if multiple Zedmons are attached.
    fn new(mut interface: InterfaceType) -> Result<Self, Error> {
        // Query parameters, disabling reporting and draining packets if a packet of the wrong type
        // is received on the first attempt.
        let parameters = match Self::get_parameters(&mut interface) {
            Err(e) => match e.downcast_ref::<protocol::Error>() {
                Some(protocol::Error::WrongPacketType { .. }) => {
                    eprintln!(
                        "WARNING: Received unexpected packet type while initializing; a prior \
                        client invocation may have not terminated cleanly. Disabling reporting and \
                        draining buffered packets. Be sure to terminate recording with ENTER."
                    );
                    Self::disable_reporting_and_drain_packets(&mut interface)?;
                    Self::get_parameters(&mut interface)
                }
                _ => Err(e),
            },
            ok => ok,
        }?;

        let field_formats = Self::get_field_formats(&mut interface)?;

        let shunt_resistance = {
            let value = parameters["shunt_resistance"];
            if let Value::F32(v) = value {
                v
            } else {
                bail!("Wrong value type for shunt_resistance: {:?}", value);
            }
        };

        // Use a HashMap to assist in field lookup for simplicity. Note that the Vec<ReportFormat>
        // representation needs to be retained for later Report-parsing.
        let formats_by_name: HashMap<String, ReportFormat> =
            field_formats.iter().map(|f| (f.name.clone(), f.clone())).collect();
        let v_shunt_index = formats_by_name["v_shunt"].index as usize;
        let v_bus_index = formats_by_name["v_bus"].index as usize;

        Ok(Self {
            interface: RefCell::new(interface),
            field_formats,
            shunt_resistance,
            v_shunt_index,
            v_bus_index,
        })
    }

    /// Retrieves a ParameterValue from the provided Zedmon interface.
    fn get_parameter(interface: &mut InterfaceType, index: u8) -> Result<ParameterValue, Error> {
        let request = protocol::encode_query_parameter(index);
        interface.write(&request)?;

        let mut response = [0; MAX_PACKET_SIZE];
        let len = interface.read(&mut response)?;

        Ok(protocol::parse_parameter_value(&mut &response[0..len])?)
    }

    /// Retrieves every ParameterValue from the provided Zedmon interface.
    fn get_parameters(interface: &mut InterfaceType) -> Result<HashMap<String, Value>, Error> {
        let mut parameters = HashMap::new();
        loop {
            let parameter = Self::get_parameter(interface, parameters.len() as u8)?;
            if parameter.name.is_empty() {
                return Ok(parameters);
            }
            parameters.insert(parameter.name, parameter.value);
        }
    }

    /// Retrieves a ReportFormat from the provided Zedmon interface.
    fn get_report_format(interface: &mut InterfaceType, index: u8) -> Result<ReportFormat, Error> {
        let request = protocol::encode_query_report_format(index);
        interface.write(&request)?;

        let mut response = [0; MAX_PACKET_SIZE];
        let len = interface.read(&mut response)?;

        Ok(protocol::parse_report_format(&response[..len])?)
    }

    /// Retrieves the ReportFormat for each Report field from the provided Zedmon interface.
    fn get_field_formats(interface: &mut InterfaceType) -> Result<Vec<ReportFormat>, Error> {
        let mut all_fields = vec![];
        loop {
            let format = Self::get_report_format(interface, all_fields.len() as u8)?;
            if format.index == protocol::REPORT_FORMAT_INDEX_END {
                return Ok(all_fields);
            }
            all_fields.push(format);
        }
    }

    /// Disables reporting on the Zedmon device.
    fn disable_reporting(interface: &mut InterfaceType) -> Result<(), Error> {
        let request = protocol::encode_disable_reporting();
        interface.write(&request)?;
        Ok(())
    }

    /// Enables reporting on the Zedmon device.
    fn enable_reporting(&self) -> Result<(), Error> {
        let request = protocol::encode_enable_reporting();
        self.interface.borrow_mut().write(&request)?;
        Ok(())
    }

    /// Reads reported data from the Zedmon device, taking care of enabling/disabling reporting.
    ///
    /// Measurement data is written to `writer`. Reporting will cease when `stopper` raises its stop
    /// signal.
    pub fn read_reports(
        &self,
        mut writer: Box<dyn Write + Send>,
        mut stopper: impl StopSignal,
    ) -> Result<(), Error> {
        // This function's workload is shared between its main thread and `output_thread`.
        //
        // The main thread enables reporting, reads USB packets via blocking reads, and sends those
        // packets to `output_thread` via `packet_sender`. When `stopper` indicates that reporting
        // should stop, it drops `packet_sender` to close the channel. `output_thread` will still
        // receive packets that have been sent before closure.
        //
        // Meanwhile, `output_thread` parses each packet it receives to a Vec<Report>, which it then
        // formats and outputs via `writer`.
        //
        // The multithreading has not been confirmed as necessary for performance reasons, but it
        // seems like a reasonable thing to do, as both reading from USB and outputting (typically
        // to stdout or a file) involve blocking on I/O.
        let (packet_sender, packet_receiver) = std::sync::mpsc::channel::<Vec<u8>>();

        // Prepare data to move into `output_thread`.
        let parser = protocol::ReportParser::new(&self.field_formats)?;
        let shunt_resistance = self.shunt_resistance;
        let v_shunt_index = self.v_shunt_index;
        let v_bus_index = self.v_bus_index;

        let output_thread = std::thread::spawn(move || -> Result<(), Error> {
            for buffer in packet_receiver.iter() {
                let reports = parser.parse_reports(&buffer).unwrap();
                for report in reports.into_iter() {
                    write!(
                        *writer,
                        "{}",
                        report_to_string(report, shunt_resistance, v_shunt_index, v_bus_index,)
                    )?;
                }
            }
            writer.flush()?;
            Ok(())
        });

        // Enable reporting and run the main loop.
        self.enable_reporting()?;
        loop {
            let mut buffer = vec![0; MAX_PACKET_SIZE];
            match self.interface.borrow_mut().read(&mut buffer) {
                Err(e) => eprintln!("USB read error: {}", e),
                Ok(bytes_read) => {
                    buffer.truncate(bytes_read);
                    packet_sender.send(buffer).unwrap();
                }
            }

            if stopper.should_stop()? {
                Self::disable_reporting(&mut self.interface.borrow_mut())?;
                drop(packet_sender);
                break;
            }
        }

        // Wait for the parsing thread to complete upon draining the channel buffer.
        output_thread.join().unwrap().unwrap();
        Ok(())
    }

    /// Number of timestamp query round trips used to determine time offset.
    const TIME_OFFSET_NUM_QUERIES: u32 = 10;

    /// Returns a tuple consisting of:
    ///   - An estimate of the offset between the Zedmon clock and the host clock, in nanoseconds.
    ///     The offset is defined such that, in the absence of drift,
    ///         `zedmon_clock + offset = host_time`.
    ///     It is typically, but not necessarily, positive. (Note that: (1) the signedness prevents
    ///     std::time::Duration from being a valid return type, and (2) i64 will suffice to
    ///     represent an offset of over 290 years in nanoseconds.)
    ///   - An estimate of the uncertainty in the offset, in nanoseconds. In the absence of clock
    ///     drift, the offset is accurate within ±uncertainty.
    // TODO(fxbug.dev/61471): Consider using microseconds instead, in correspondence with Zedmon
    // timestamp units.
    pub fn get_time_offset_nanos(&self) -> Result<(i64, i64), Error> {
        let mut interface = self.interface.borrow_mut();

        // For each query, we estimate that the retrieved timestamp reflects Zedmon's clock halfway
        // between the duration spanned by our QueryTime request and Zedmon's Timestamp response.
        // That allows us to estimate the offset between the host clock and Zedmon's.
        //
        // We run `TIME_OFFSET_NUM_QUERIES` queries and keep the estimate corresponding to the
        // shortest query duration. That provides some guarding against transient sources of
        // latency.
        //
        // Note: Unlike other methods that interact with the USB interface, this method does not
        // separate out a "get_timestamp" function to keep the parsing time from contributing to
        // transit time.
        let mut best_offset_nanos = 0;
        let mut best_query_duration_nanos = i64::MAX;

        for _ in 0..Self::TIME_OFFSET_NUM_QUERIES {
            let request = protocol::encode_query_time();
            let mut response = [0u8; MAX_PACKET_SIZE];

            let host_clock_at_start = SystemTime::now();
            interface.write(&request)?;
            let len = interface.read(&mut response)?;

            // `elapsed` could return an error if the host clock moved backwards. That should be
            // rare, but if it does occur, throw out this query. (Note, however, that the offset
            // would be invalidated by future jumps in the system clock.)
            let query_duration_nanos = match host_clock_at_start.elapsed() {
                Ok(duration) => duration.as_nanos() as i64,
                Err(_) => continue,
            };

            if query_duration_nanos < best_query_duration_nanos {
                let timestamp_micros = protocol::parse_timestamp_micros(&response[..len])? as i64;
                let host_nanos_at_timestamp =
                    host_clock_at_start.duration_since(SystemTime::UNIX_EPOCH)?.as_nanos() as i64
                        + query_duration_nanos / 2;
                best_offset_nanos = host_nanos_at_timestamp - timestamp_micros * 1_000;
                best_query_duration_nanos = query_duration_nanos;
            }
        }

        // Uncertainty comes from two sources:
        //  - Mapping a microsecond Zedmon timestamp to nanoseconds. In the worst case, the
        //    timestamp is 999ns stale.
        //  - Estimating the instant of the timestamp in the query interval. Since we used the
        //    midpoint, at worst we're off by one half the query duration.
        let uncertainty_nanos = 999 + best_query_duration_nanos / 2 + best_query_duration_nanos % 2;

        Ok((best_offset_nanos, uncertainty_nanos))
    }

    /// Enables or disables the relay on the Zedmon device.
    pub fn set_relay(&self, enable: bool) -> Result<(), Error> {
        let request = protocol::encode_set_output(protocol::Output::Relay as u8, enable);
        self.interface.borrow_mut().write(&request)?;
        Ok(())
    }
}

/// Lists the serial numbers of all connected Zedmons.
pub fn list() -> Vec<String> {
    ZedmonClient::<usb_bulk::Interface>::enumerate()
}

pub fn zedmon() -> ZedmonClient<usb_bulk::Interface> {
    let interface = usb_bulk::Interface::open(&mut zedmon_match).unwrap();
    let result = ZedmonClient::new(interface);
    if result.is_err() {
        eprintln!("Error initializing ZedmonClient: {:?}", result);
    }
    result.unwrap()
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::{format_err, Error},
        num::FromPrimitive,
        protocol::{tests::serialize_reports, PacketType, Report, ScalarType},
        std::collections::VecDeque,
        std::rc::Rc,
        std::sync::{
            atomic::{AtomicBool, Ordering},
            mpsc, Arc,
        },
        std::time::Duration,
        test_util::assert_near,
    };

    // Used by `interface_info`, below, as a convenient means of constructing InterfaceInfo.
    struct ShortInterface<'a> {
        dev_vendor: ::std::os::raw::c_ushort,
        dev_product: ::std::os::raw::c_ushort,
        ifc_class: ::std::os::raw::c_uchar,
        ifc_subclass: ::std::os::raw::c_uchar,
        ifc_protocol: ::std::os::raw::c_uchar,
        serial_number: &'a str,
    }

    fn interface_info(short: ShortInterface<'_>) -> InterfaceInfo {
        let mut serial = [0; 256];
        for (i, c) in short.serial_number.as_bytes().iter().enumerate() {
            serial[i] = *c;
        }

        InterfaceInfo {
            dev_vendor: short.dev_vendor,
            dev_product: short.dev_product,
            dev_class: 0,
            dev_subclass: 0,
            dev_protocol: 0,
            ifc_class: short.ifc_class,
            ifc_subclass: short.ifc_subclass,
            ifc_protocol: short.ifc_protocol,
            has_bulk_in: 0,
            has_bulk_out: 0,
            writable: 0,
            serial_number: serial,
            device_path: [0; 256usize],
        }
    }

    #[test]
    fn test_enumerate() {
        // AVAILABLE_DEVICES is state for the static method FakeEnumerationInterface::open. This
        // test is single-threaded, so a thread-local static provides the most appropriate safe
        // interface.
        thread_local! {
            static AVAILABLE_DEVICES: RefCell<Vec<InterfaceInfo>> = RefCell::new(Vec::new());
        }
        fn push_device(short: ShortInterface<'_>) {
            AVAILABLE_DEVICES.with(|devices| {
                devices.borrow_mut().push(interface_info(short));
            });
        }

        struct FakeEnumerationInterface {}

        impl usb_bulk::Open<FakeEnumerationInterface> for FakeEnumerationInterface {
            fn open<F>(matcher: &mut F) -> Result<FakeEnumerationInterface, Error>
            where
                F: FnMut(&InterfaceInfo) -> bool,
            {
                AVAILABLE_DEVICES.with(|devices| {
                    let devices = devices.borrow();
                    for device in devices.iter() {
                        if matcher(device) {
                            return Ok(FakeEnumerationInterface {});
                        }
                    }
                    Err(format_err!("No matching devices found."))
                })
            }
        }

        impl Read for FakeEnumerationInterface {
            fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
                Ok(0)
            }
        }

        impl Write for FakeEnumerationInterface {
            fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
                Ok(0)
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

        // No devices connected
        let serials = ZedmonClient::<FakeEnumerationInterface>::enumerate();
        assert!(serials.is_empty());

        // One device: not-a-zedmon-1
        push_device(ShortInterface {
            dev_vendor: 0xdead,
            dev_product: ZEDMON_PRODUCT_ID,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "not-a-zedmon-1",
        });
        let serials = ZedmonClient::<FakeEnumerationInterface>::enumerate();
        assert!(serials.is_empty());

        // Two devices: not-a-zedmon-1, zedmon-1
        push_device(ShortInterface {
            dev_vendor: GOOGLE_VENDOR_ID,
            dev_product: ZEDMON_PRODUCT_ID,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "zedmon-1",
        });
        let serials = ZedmonClient::<FakeEnumerationInterface>::enumerate();
        assert_eq!(serials, ["zedmon-1"]);

        // Three devices: not-a-zedmon-1, zedmon-1, not-a-zedmon-2
        push_device(ShortInterface {
            dev_vendor: GOOGLE_VENDOR_ID,
            dev_product: 0xbeef,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "not-a-zedmon-2",
        });
        let serials = ZedmonClient::<FakeEnumerationInterface>::enumerate();
        assert_eq!(serials, ["zedmon-1"]);

        // Four devices: not-a-zedmon-1, zedmon-1, not-a-zedmon-2, zedmon-2
        push_device(ShortInterface {
            dev_vendor: GOOGLE_VENDOR_ID,
            dev_product: ZEDMON_PRODUCT_ID,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "zedmon-2",
        });
        let serials = ZedmonClient::<FakeEnumerationInterface>::enumerate();
        assert_eq!(serials, ["zedmon-1", "zedmon-2"]);
    }

    // Provides test support for ZedmonClient functionality that interacts with a Zedmon device. It
    // chiefly provides:
    //  - FakeZedmonInterface, for faking ZedmonClient's access to a Zedmon device.
    //  - Coordinator, for coordinating activity between the test and a FakeZedmonInterface.
    //  - CoordinatorBuilder, for producing a Coordinator instance with its various optional
    //    settings.
    mod fake_device {
        use {
            super::*,
            num::FromPrimitive,
            protocol::{tests::*, PacketType, Unit},
        };

        // StopSignal implementer for testing ZedmonClient::read_reports. The state is set by a
        // Coordinator.
        pub struct Stopper {
            signal: Arc<AtomicBool>,
        }

        impl StopSignal for Stopper {
            fn should_stop(&mut self) -> Result<bool, Error> {
                return Ok(self.signal.load(Ordering::SeqCst));
            }
        }

        // Coordinates interactions between FakeZedmonInterface and a test.
        //
        // To test ZedmonClient::read_reports:
        //  - Use CoordinatorBuilder::with_report_queue to populate the `report_queue` field. Each
        //    entry in the queue is a Vec<Report> that will be serialized into a single packet. The
        //    caller will need to ensure that reports are written to an accessible location.
        //  - Use CoordinatorHandle::get_stopper to build a Stopper for `read_reports` that will
        //    end reporting when the report queue is exhausted.
        //
        // To test ZedmonClient::get_time_offset_nanos, use CoordinatorBuilder::with_offset_time to
        // populate `offset_time`. Timestamps will be reported as `host_time - offset_time`; the
        // fake Zedmon's clock will run perfectly in parallel to the host clock. Note that only
        // timestamps reported in Timestamp packets are affected by `offset_time`; timestamps in
        // Report packets are directly specified by the test, via `report_queue`.
        //
        // To test ZedmonClient::set_relay, use CoordinatorBuilder::with_relay_enabled to
        // populate the relay state, and CoordinatorHandle::relay_enabled to check expectations.
        pub struct Coordinator {
            // Constants that define the fake device.
            device_config: DeviceConfiguration,

            // See struct comment.
            report_queue: Option<VecDeque<Vec<Report>>>,

            // Signal to trigger the end of reporting. Initialized to `false`, and set to `true`
            // when `report_queue` is exhausted.
            stop_signal: Arc<AtomicBool>,

            // Offset between the fake Zedmon's clock and host clock, such that `zedmon_time +
            // offset_time = host_time`. Only used for Timestamp packets; the timestamps in
            // Reports are set by the test when populating `report_queue`.
            offset_time: Option<Duration>,

            // Whether Zedmon's relay is enabled.
            relay_enabled: Option<bool>,
        }

        impl Coordinator {
            pub fn get_stopper(&self) -> Stopper {
                Stopper { signal: self.stop_signal.clone() }
            }

            pub fn relay_enabled(&self) -> bool {
                self.relay_enabled.expect("relay_enabled not set")
            }

            fn get_device_config(&self) -> DeviceConfiguration {
                self.device_config.clone()
            }

            // Gets the next packet's worth of Reports.
            fn get_reports_for_packet(&mut self) -> Vec<Report> {
                let report_queue = self.report_queue.as_mut().expect("report_queue not set");

                assert!(report_queue.len() > 0, "No reports left in queue");
                if report_queue.len() == 1 {
                    self.stop_signal.store(true, Ordering::SeqCst);
                }
                report_queue.pop_front().unwrap()
            }

            // Retrieves a timestamp in microseconds to fill a Timestamp packet.
            fn get_timestamp_micros(&self) -> u64 {
                let offset_time = self.offset_time.expect("offset_time not set");

                let zedmon_now = SystemTime::now() - offset_time;
                let timestamp = zedmon_now.duration_since(SystemTime::UNIX_EPOCH).unwrap();
                timestamp.as_micros() as u64
            }

            fn set_relay_enabled(&mut self, enabled: bool) {
                let relay_enabled = self.relay_enabled.as_mut().expect("relay_enabled not set");
                *relay_enabled = enabled;
            }
        }

        // Constants that are inherent to a Zedmon device. Even if these values are not exercised by
        // a test, dummy values will be required by ZedmonClient::new().
        #[derive(Clone, Debug)]
        pub struct DeviceConfiguration {
            pub shunt_resistance: f32,
            pub v_shunt_scale: f32,
            pub v_bus_scale: f32,
        }

        // Provides the interface for building a Coordinator with its various optional settings.
        pub struct CoordinatorBuilder {
            device_config: DeviceConfiguration,
            report_queue: Option<VecDeque<Vec<Report>>>,
            offset_time: Option<Duration>,
            relay_enabled: Option<bool>,
        }

        impl CoordinatorBuilder {
            pub fn new(device_config: DeviceConfiguration) -> Self {
                CoordinatorBuilder {
                    device_config,
                    report_queue: None,
                    offset_time: None,
                    relay_enabled: None,
                }
            }

            pub fn with_report_queue(mut self, report_queue: VecDeque<Vec<Report>>) -> Self {
                self.report_queue.replace(report_queue);
                self
            }

            pub fn with_offset_time(mut self, offset_time: Duration) -> Self {
                self.offset_time.replace(offset_time);
                self
            }

            pub fn with_relay_enabled(mut self, enabled: bool) -> Self {
                self.relay_enabled.replace(enabled);
                self
            }

            pub fn build(self) -> Rc<RefCell<Coordinator>> {
                Rc::new(RefCell::new(Coordinator {
                    device_config: self.device_config,
                    report_queue: self.report_queue,
                    offset_time: self.offset_time,
                    relay_enabled: self.relay_enabled,
                    stop_signal: Arc::new(AtomicBool::new(false)),
                }))
            }
        }

        // Indicates the contents of the next read from FakeZedmonInterface.
        enum NextRead {
            ParameterValue(u8),
            ReportFormat(u8),
            Report,
            Timestamp,
        }

        // Interface that provides fakes for testing interactions with a Zedmon device.
        pub struct FakeZedmonInterface {
            coordinator: Rc<RefCell<Coordinator>>,

            // The type of read that wil be performed next from this interface, if any.
            next_read: Option<NextRead>,
        }

        impl usb_bulk::Open<FakeZedmonInterface> for FakeZedmonInterface {
            fn open<F>(_matcher: &mut F) -> Result<FakeZedmonInterface, Error>
            where
                F: FnMut(&InterfaceInfo) -> bool,
            {
                Err(format_err!("usb_bulk::Open not implemented"))
            }
        }

        impl FakeZedmonInterface {
            pub fn new(coordinator: Rc<RefCell<Coordinator>>) -> Self {
                Self { coordinator, next_read: None }
            }

            // Populates a ParameterValue packet.
            fn read_parameter_value(&mut self, index: u8, buffer: &mut [u8]) -> usize {
                match index {
                    0 => serialize_parameter_value(
                        ParameterValue {
                            name: "shunt_resistance".to_string(),
                            value: Value::F32(
                                self.coordinator.borrow().get_device_config().shunt_resistance,
                            ),
                        },
                        buffer,
                    ),
                    1 => serialize_parameter_value(
                        ParameterValue { name: "".to_string(), value: Value::U8(0) },
                        buffer,
                    ),
                    _ => panic!("Should only receive 0 or 1 as indices"),
                }
            }

            // Populates a ReportFormat packet.
            fn read_report_format(&self, index: u8, buffer: &mut [u8]) -> usize {
                match index {
                    0 => serialize_report_format(
                        ReportFormat {
                            index,
                            field_type: ScalarType::I16,
                            unit: Unit::Volts,
                            scale: self.coordinator.borrow().get_device_config().v_shunt_scale,
                            name: "v_shunt".to_string(),
                        },
                        buffer,
                    ),
                    1 => serialize_report_format(
                        ReportFormat {
                            index,
                            field_type: ScalarType::I16,
                            unit: Unit::Volts,
                            scale: self.coordinator.borrow().get_device_config().v_bus_scale,
                            name: "v_bus".to_string(),
                        },
                        buffer,
                    ),
                    2 => serialize_report_format(
                        ReportFormat {
                            index: protocol::REPORT_FORMAT_INDEX_END,
                            field_type: ScalarType::U8,
                            unit: Unit::Volts,
                            scale: 0.0,
                            name: "".to_string(),
                        },
                        buffer,
                    ),
                    _ => panic!("Should only receive 0, 1, or 2 as indices"),
                }
            }

            // Populates a Report packet.
            fn read_reports(&mut self, buffer: &mut [u8]) -> usize {
                let reports = self.coordinator.borrow_mut().get_reports_for_packet();
                serialize_reports(&reports, buffer)
            }

            // Populates a Timestamp packet.
            fn read_timestamp(&mut self, buffer: &mut [u8]) -> usize {
                buffer[0] = PacketType::Timestamp as u8;
                serialize_timestamp_micros(self.coordinator.borrow().get_timestamp_micros(), buffer)
            }

            fn set_output(&self, index: u8, value: u8) {
                if index == protocol::Output::Relay as u8 {
                    self.coordinator.borrow_mut().set_relay_enabled(value != 0);
                }
            }
        }

        impl Read for FakeZedmonInterface {
            fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
                match self.next_read.take() {
                    Some(value) => Ok(match value {
                        NextRead::ParameterValue(index) => self.read_parameter_value(index, buffer),
                        NextRead::ReportFormat(index) => self.read_report_format(index, buffer),
                        NextRead::Report => {
                            self.next_read = Some(NextRead::Report);
                            self.read_reports(buffer)
                        }
                        NextRead::Timestamp => self.read_timestamp(buffer),
                    }),
                    None => Err(std::io::Error::new(std::io::ErrorKind::Other, "Read error: -1")),
                }
            }
        }

        impl Write for FakeZedmonInterface {
            fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
                let packet_type = PacketType::from_u8(data[0]).unwrap();
                match packet_type {
                    PacketType::EnableReporting => self.next_read = Some(NextRead::Report),
                    PacketType::DisableReporting => self.next_read = None,
                    PacketType::QueryParameter => {
                        self.next_read = Some(NextRead::ParameterValue(data[1]))
                    }
                    PacketType::QueryReportFormat => {
                        self.next_read = Some(NextRead::ReportFormat(data[1]))
                    }
                    PacketType::QueryTime => self.next_read = Some(NextRead::Timestamp),
                    PacketType::SetOutput => {
                        assert_eq!(data.len(), 3);
                        self.set_output(data[1], data[2]);
                    }
                    _ => panic!("Not a valid host-to-target packet"),
                }
                Ok(data.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
    }

    // Helper struct for testing ZedmonClient::record.
    struct ZedmonRecordRunner {
        // Maps time in microseconds to (v_shunt, v_bus).
        voltage_function: Box<dyn Fn(u64) -> (f32, f32)>,

        shunt_resistance: f32,
        v_shunt_scale: f32,
        v_bus_scale: f32,

        // The length of the test, and interval between report timestamps. The first report is sent
        // one interval after the starting instant.
        test_duration: Duration,
        reporting_interval: Duration,
    }

    impl ZedmonRecordRunner {
        fn run(&self) -> Result<String, Error> {
            let device_config = fake_device::DeviceConfiguration {
                shunt_resistance: self.shunt_resistance,
                v_shunt_scale: self.v_shunt_scale,
                v_bus_scale: self.v_bus_scale,
            };

            let mut report_queue = VecDeque::new();

            let mut elapsed = Duration::from_millis(0);

            // 1ms of fake time elapses between each report. Reports are batched into groups of 5,
            // the number that will fit into a single packet.
            while elapsed <= self.test_duration {
                let mut reports = Vec::new();
                for _ in 0..5 {
                    elapsed = elapsed + self.reporting_interval;
                    if elapsed > self.test_duration {
                        break;
                    }

                    let (v_shunt, v_bus) = (self.voltage_function)(elapsed.as_micros() as u64);
                    reports.push(Report {
                        timestamp_micros: elapsed.as_micros() as u64,
                        values: vec![
                            Value::I16((v_shunt / self.v_shunt_scale) as i16),
                            Value::I16((v_bus / self.v_bus_scale) as i16),
                        ],
                    });
                }
                if !reports.is_empty() {
                    report_queue.push_back(reports);
                }
            }

            let builder =
                fake_device::CoordinatorBuilder::new(device_config).with_report_queue(report_queue);
            let coordinator = builder.build();
            let interface = fake_device::FakeZedmonInterface::new(coordinator.clone());
            let zedmon = ZedmonClient::new(interface)?;

            // Implements Write by sending bytes over a channel. The holder of the channel's
            // Receiver can then inspect the data that was written to test expectations.
            struct ChannelWriter {
                sender: mpsc::Sender<Vec<u8>>,
                buffer: Vec<u8>,
            }

            impl std::io::Write for ChannelWriter {
                fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                    self.buffer.extend_from_slice(buf);
                    Ok(buf.len())
                }

                fn flush(&mut self) -> std::io::Result<()> {
                    let mut payload = Vec::new();
                    std::mem::swap(&mut self.buffer, &mut payload);
                    self.sender.send(payload).unwrap();
                    Ok(())
                }
            }

            let (sender, receiver) = mpsc::channel();
            let writer = Box::new(ChannelWriter { sender, buffer: Vec::new() });
            let stopper = coordinator.borrow().get_stopper();
            zedmon.read_reports(writer, stopper)?;

            let output = receiver.recv()?;
            Ok(String::from_utf8(output)?)
        }
    }

    // Tests that ZedmonClient will disable reporting and drain enqueued packets on initialization.
    #[test]
    fn test_disable_reporting_and_drain_packets() {
        // Interface that responds to reads with Report packets until reporting is disabled and an
        // enqueued packet is drained. Afterwards, all calls are forwarded to an inner
        // FakeZedmonInterface.
        struct StillReportingInterface {
            inner: fake_device::FakeZedmonInterface,
            reporting_enabled: bool,
            packets_enqueued: usize,
        }

        impl StillReportingInterface {
            fn new(coordinator: Rc<RefCell<fake_device::Coordinator>>) -> Self {
                Self {
                    inner: fake_device::FakeZedmonInterface::new(coordinator),
                    reporting_enabled: true,
                    packets_enqueued: 1,
                }
            }

            fn make_reports(&self) -> Vec<Report> {
                let mut reports = Vec::new();
                for i in 0..5 {
                    reports.push(Report {
                        timestamp_micros: 1000 * (i as u64),
                        values: vec![Value::I16(i as i16), Value::I16(-(i as i16))],
                    });
                }
                reports
            }
        }

        impl usb_bulk::Open<StillReportingInterface> for StillReportingInterface {
            fn open<F>(_matcher: &mut F) -> Result<StillReportingInterface, Error>
            where
                F: FnMut(&InterfaceInfo) -> bool,
            {
                Err(format_err!("usb_bulk::Open not implemented"))
            }
        }

        impl Read for StillReportingInterface {
            fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
                if self.reporting_enabled {
                    Ok(serialize_reports(&self.make_reports(), buffer))
                } else if self.packets_enqueued > 0 {
                    self.packets_enqueued = self.packets_enqueued - 1;
                    Ok(serialize_reports(&self.make_reports(), buffer))
                } else {
                    self.inner.read(buffer)
                }
            }
        }

        impl Write for StillReportingInterface {
            fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
                if self.reporting_enabled
                    && PacketType::from_u8(data[0]).unwrap() == PacketType::DisableReporting
                {
                    self.reporting_enabled = false;
                    Ok(1)
                } else {
                    self.inner.write(data)
                }
            }

            fn flush(&mut self) -> std::io::Result<()> {
                self.inner.flush()
            }
        }

        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.01,
            v_shunt_scale: 1e-5,
            v_bus_scale: 0.025,
        };
        let builder = fake_device::CoordinatorBuilder::new(device_config);
        let coordinator = builder.build();
        let interface = StillReportingInterface::new(coordinator);

        assert!(ZedmonClient::new(interface).is_ok());
    }

    #[test]
    fn test_read_reports() -> Result<(), Error> {
        // The voltages used are simply time-dependent signals that, combined with shunt_resistance
        // below, yield power on the order of a few Watts.
        fn get_voltages(micros: u64) -> (f32, f32) {
            let seconds = micros as f32 / 1e6;
            let v_shunt = 1e-3 + 2e-4 * (std::f32::consts::PI * seconds).cos();
            let v_bus = 20.0 + 3.0 * (std::f32::consts::PI * seconds).sin();
            (v_shunt, v_bus)
        }

        // These values are in the same ballpark as those used on Zedmon 2.1. The test shouldn't be
        // sensitive to them.
        let shunt_resistance = 0.01;
        let v_shunt_scale = 1e-5;
        let v_bus_scale = 0.025;

        let runner = ZedmonRecordRunner {
            voltage_function: Box::new(get_voltages),
            shunt_resistance,
            v_shunt_scale,
            v_bus_scale,
            test_duration: Duration::from_secs(10),
            reporting_interval: Duration::from_millis(1),
        };
        let output = runner.run()?;

        let mut num_lines = 0;
        for line in output.lines() {
            num_lines = num_lines + 1;

            let parts: Vec<&str> = line.split(",").collect();
            assert_eq!(4, parts.len());
            let timestamp: u64 = parts[0].parse()?;
            let v_shunt_out: f32 = parts[1].parse()?;
            let v_bus_out: f32 = parts[2].parse()?;

            let (v_shunt_expected, v_bus_expected) = get_voltages(timestamp);
            assert_near!(v_shunt_out, v_shunt_expected, v_shunt_scale);
            assert_near!(v_bus_out, v_bus_expected, v_bus_scale);

            let power_out: f32 = parts[3].parse()?;
            assert_near!(power_out, v_shunt_out * v_bus_out / shunt_resistance, 1e-6);
        }

        assert_eq!(num_lines, 10000);

        Ok(())
    }

    #[test]
    fn test_get_time_offset_nanos() -> Result<(), Error> {
        // This instant is effectively Zedmon's zero timestamp.
        let zedmon_offset = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?;

        // Values are not used by this test.
        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.0,
            v_shunt_scale: 0.0,
            v_bus_scale: 0.0,
        };

        let builder =
            fake_device::CoordinatorBuilder::new(device_config).with_offset_time(zedmon_offset);
        let coordinator = builder.build();
        let interface = fake_device::FakeZedmonInterface::new(coordinator);
        let zedmon = ZedmonClient::new(interface)?;

        let (reported_offset, uncertainty) = zedmon.get_time_offset_nanos()?;
        assert_near!(zedmon_offset.as_nanos() as i64, reported_offset, uncertainty);

        Ok(())
    }

    #[test]
    fn test_set_relay() -> Result<(), Error> {
        // Values are not used by this test.
        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.0,
            v_shunt_scale: 0.0,
            v_bus_scale: 0.0,
        };

        let builder = fake_device::CoordinatorBuilder::new(device_config).with_relay_enabled(false);
        let coordinator = builder.build();
        let interface = fake_device::FakeZedmonInterface::new(coordinator.clone());
        let zedmon = ZedmonClient::new(interface)?;

        // Test true->false and false->true transitions, and no-ops in each state.
        zedmon.set_relay(true)?;
        assert_eq!(coordinator.borrow().relay_enabled(), true);
        zedmon.set_relay(true)?;
        assert_eq!(coordinator.borrow().relay_enabled(), true);
        zedmon.set_relay(false)?;
        assert_eq!(coordinator.borrow().relay_enabled(), false);
        zedmon.set_relay(false)?;
        assert_eq!(coordinator.borrow().relay_enabled(), false);
        zedmon.set_relay(true)?;
        assert_eq!(coordinator.borrow().relay_enabled(), true);

        Ok(())
    }
}
