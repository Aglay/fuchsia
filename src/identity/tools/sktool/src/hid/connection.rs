// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::hid::message::Packet;
use async_trait::async_trait;
use bytes::Bytes;
use failure::Error;
use std::fmt::Debug;

#[cfg(test)]
pub use self::fake::FakeConnection;
pub use self::fidl::FidlConnection;

/// A basic connection to a HID device.
#[async_trait(?Send)]
pub trait Connection: Sized + Debug {
    /// Receives a single CTAP packet (aka HID report) from the device.
    async fn read_packet(&self) -> Result<Packet, Error>;

    /// Writes a single CTAP packet (aka HID report) to the device.
    async fn write_packet(&self, packet: Packet) -> Result<(), Error>;

    /// Returns the report descriptor for this device.
    async fn report_descriptor(&self) -> Result<Bytes, Error>;

    /// Returns the maximum size of packets this device can read.
    async fn max_packet_size(&self) -> Result<u16, Error>;
}

/// An implementation of a `Connection` over the FIDL `fuchsia.hardware.input.Device` protocol.
pub mod fidl {
    use crate::hid::connection::Connection;
    use crate::hid::message::Packet;
    use async_trait::async_trait;
    use bytes::Bytes;
    use failure::{format_err, Error};
    use fidl_fuchsia_hardware_input::{DeviceProxy, ReportType};
    use fuchsia_zircon as zx;
    use std::convert::TryFrom;

    // TODO(jsankey): Don't hardcode the report IDs, although its hard to imagine other values
    const OUTPUT_REPORT_ID: u8 = 0;
    #[allow(dead_code)]
    const INPUT_REPORT_ID: u8 = 0;

    /// An connection to a HID device over the FIDL `Device` protocol.
    #[derive(Debug)]
    pub struct FidlConnection {
        proxy: DeviceProxy,
    }

    impl FidlConnection {
        /// Constructs a new `FidlConnection` using the supplied `DeviceProxy`.
        pub fn new(proxy: DeviceProxy) -> FidlConnection {
            FidlConnection { proxy }
        }
    }

    #[async_trait(?Send)]
    impl Connection for FidlConnection {
        async fn read_packet(&self) -> Result<Packet, Error> {
            // TODO(jsankey): Currently this requests reports that have already been received and
            // returns ZX_ERR_SHOULD_WAIT if none are available. Once this simple case is working
            // reliably, expand the implementation to use GetReportsEvent to await for signalling
            // on an event when a message arrives, potentially changing the method signature to
            // accept a timeout.
            let (status, data) = self
                .proxy
                .get_reports()
                .await
                .map_err(|err| format_err!("FIDL error reading packet: {:?}", err))
                .map(|(status, data)| (zx::Status::from_raw(status), data))?;
            if status != zx::Status::OK {
                return Err(format_err!("Received not-ok status reading packet: {:?}", status));
            }
            Packet::try_from(data)
        }

        async fn write_packet(&self, packet: Packet) -> Result<(), Error> {
            match self
                .proxy
                .set_report(ReportType::Output, OUTPUT_REPORT_ID, &mut packet.into_iter())
                .await
                .map_err(|err| format_err!("FIDL error writing packet: {:?}", err))
                .map(|status| zx::Status::from_raw(status))?
            {
                zx::Status::OK => Ok(()),
                s => Err(format_err!("Received not-ok status sending packet: {:?}", s)),
            }
        }

        async fn report_descriptor(&self) -> Result<Bytes, Error> {
            self.proxy
                .get_report_desc()
                .await
                .map(|vec| Bytes::from(vec))
                .map_err(|err| format_err!("FIDL error: {:?}", err))
        }

        async fn max_packet_size(&self) -> Result<u16, Error> {
            self.proxy
                .get_max_input_report_size()
                .await
                .map_err(|err| format_err!("FIDL error: {:?}", err))
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use fidl::endpoints::{create_proxy_and_stream, RequestStream};
        use fidl_fuchsia_hardware_input::{DeviceMarker, DeviceRequest};
        use fuchsia_async as fasync;
        use futures::TryStreamExt;

        const TEST_REPORT_SIZE: u16 = 99;
        const TEST_REPORT_DESCRIPTOR: [u8; 8] = [0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01, 0x09];
        const TEST_PACKET: [u8; 9] = [0xfe, 0xef, 0xbc, 0xcb, 0x86, 0x00, 0x02, 0x88, 0x99];

        /// Creates a mock device proxy that will invoke the supplied function on each request.
        fn valid_mock_device_proxy<F>(request_fn: F) -> DeviceProxy
        where
            F: (Fn(DeviceRequest) -> ()) + Send + 'static,
        {
            let (device_proxy, mut stream) = create_proxy_and_stream::<DeviceMarker>()
                .expect("Failed to create proxy and stream");
            fasync::spawn(async move {
                while let Some(req) = stream.try_next().await.expect("Failed to read req") {
                    request_fn(req)
                }
            });
            device_proxy
        }

        /// Creates a mock device proxy that will immediately close the channel.
        fn invalid_mock_device_proxy() -> DeviceProxy {
            let (device_proxy, stream) = create_proxy_and_stream::<DeviceMarker>()
                .expect("Failed to create proxy and stream");
            stream.control_handle().shutdown();
            device_proxy
        }

        #[fasync::run_until_stalled(test)]
        async fn test_read_packet() -> Result<(), Error> {
            let proxy = valid_mock_device_proxy(|req| match req {
                DeviceRequest::GetReports { responder } => {
                    let response = TEST_PACKET.to_vec();
                    responder
                        .send(zx::sys::ZX_OK, &mut response.into_iter())
                        .expect("failed to send response");
                }
                _ => panic!("got unexpected device request."),
            });
            let connection = FidlConnection::new(proxy);
            assert_eq!(connection.read_packet().await?, Packet::try_from(TEST_PACKET.to_vec())?);
            Ok(())
        }

        #[fasync::run_until_stalled(test)]
        async fn test_write_packet() -> Result<(), Error> {
            let proxy = valid_mock_device_proxy(|req| match req {
                DeviceRequest::SetReport {
                    type_: ReportType::Output,
                    id: 0,
                    report,
                    responder,
                } => {
                    if report != &TEST_PACKET[..] {
                        panic!("received unexpected packet.")
                    }
                    responder.send(zx::sys::ZX_OK).expect("failed to send response");
                }
                _ => panic!("got unexpected device request."),
            });
            let connection = FidlConnection::new(proxy);
            connection.write_packet(Packet::try_from(TEST_PACKET.to_vec())?).await
        }

        #[fasync::run_until_stalled(test)]
        async fn test_report_descriptor() -> Result<(), Error> {
            let proxy = valid_mock_device_proxy(|req| match req {
                DeviceRequest::GetReportDesc { responder } => {
                    let response = TEST_REPORT_DESCRIPTOR.to_vec();
                    responder.send(&mut response.into_iter()).expect("failed to send response");
                }
                _ => panic!("got unexpected device request."),
            });
            let connection = FidlConnection::new(proxy);
            assert_eq!(connection.report_descriptor().await?, &TEST_REPORT_DESCRIPTOR[..]);
            Ok(())
        }

        #[fasync::run_until_stalled(test)]
        async fn test_max_packet_size() -> Result<(), Error> {
            let proxy = valid_mock_device_proxy(|req| match req {
                DeviceRequest::GetMaxInputReportSize { responder } => {
                    responder.send(TEST_REPORT_SIZE).expect("failed to send response");
                }
                _ => panic!("got unexpected device request."),
            });
            let connection = FidlConnection::new(proxy);
            assert_eq!(connection.max_packet_size().await?, TEST_REPORT_SIZE);
            Ok(())
        }

        #[fasync::run_until_stalled(test)]
        async fn test_fidl_error() -> Result<(), Error> {
            let connection = FidlConnection::new(invalid_mock_device_proxy());
            connection.report_descriptor().await.expect_err("Should have failed to get descriptor");
            connection.max_packet_size().await.expect_err("Should have failed to get packet size");
            Ok(())
        }
  }
}

/// A fake implementation of a `Connection` to simplify unit testing.
#[cfg(test)]
pub mod fake {
    use crate::hid::connection::Connection;
    use crate::hid::message::Packet;
    use async_trait::async_trait;
    use bytes::Bytes;
    use failure::{format_err, Error};
    use fuchsia_async::futures::lock::Mutex;
    use std::collections::VecDeque;

    /// A fixed packet size used by all devices.
    const REPORT_SIZE: u16 = 64;

    /// A single operation for a fake connection
    #[derive(Debug)]
    enum Operation {
        /// Expect a call to write the specified packet and return success.
        WriteSuccess(Packet),
        /// Expect a call to write the specified packet and return an error.
        WriteFail(Packet),
        /// Expect a call to read a packet and return the supplied data packet.
        ReadSuccess(Packet),
        /// Expect a call to read a packet and return an error.
        ReadFail(),
    }

    /// The mode that a fake connection should operate in.
    #[derive(Debug)]
    enum Mode {
        /// The connection returns errors on all calls.
        Invalid,
        /// The connection potentially returns valid data.
        Valid {
            /// The report descriptor to return.
            report_desc: Bytes,
            /// A queue of expected operations and intended responses.
            operations: Mutex<VecDeque<Operation>>,
        },
    }

    /// A fake implmentation of a `Connection` to a HID device to simplify unit testing.
    ///
    /// `FakeConnections` may either be set to be invalid, in which casse they return errors for
    /// all calls, or valid. A valid `FakeConnection` can perform read and write operations
    /// following an expected set of operations supplied by the test code using it. If a
    /// `FakeConnection` receives requests that do not align with the expection it panics.
    #[derive(Debug)]
    pub struct FakeConnection {
        /// The current operational state of the connection.
        mode: Mode,
    }

    impl FakeConnection {
        /// Constructs a new `FidlConnection` that will return the supplied report_descriptor.
        pub fn new(report_descriptor: &'static [u8]) -> FakeConnection {
            FakeConnection {
                mode: Mode::Valid {
                    report_desc: Bytes::from(report_descriptor),
                    operations: Mutex::new(VecDeque::new()),
                },
            }
        }

        /// Sets all further calls on this FakeConnection to return errors.
        /// This simulates the behavior of a connection to a device that has been removed.
        pub fn fail(&mut self) {
            self.expect_complete();
            self.mode = Mode::Invalid;
        }

        /// Enqueues a single operation, provided the connection is valid.
        /// Panics if called on a connection that has been set to fail.
        fn enqueue(&self, operation: Operation) {
            match &self.mode {
                Mode::Valid { operations, .. } => {
                    operations.try_lock().unwrap().push_back(operation);
                }
                Mode::Invalid => panic!("Cannot queue operations on a failing FakeConnection"),
            }
        }

        /// Enqueues an expectation that write will be called on this connection with the supplied
        /// packet. The connection will return success when this write operation occurs.
        /// Panics if called on a connection that has been set to fail.
        pub fn expect_write(&self, packet: Packet) {
            &self.enqueue(Operation::WriteSuccess(packet));
        }

        /// Enqueues an expectation that write will be called on this connection with the supplied
        /// packet. The connection will return a failure when this write operation occurs.
        /// Panics if called on a connection that has been set to fail.
        pub fn expect_write_fail(&self, packet: Packet) {
            &self.enqueue(Operation::WriteFail(packet));
        }

        /// Enqueues an expectation that read will be called on this connection. The connection
        /// will return success and the supplied packet when this read operation occurs.
        /// Panics if called on a connection that has been set to fail.
        pub fn expect_read(&self, packet: Packet) {
            &self.enqueue(Operation::ReadSuccess(packet));
        }

        /// Enqueues an expectation that read will be called on this connection. The connection
        /// will return a failure when this write operation occurs.
        /// Panics if called on a connection that has been set to fail.
        pub fn expect_read_fail(&self) {
            &self.enqueue(Operation::ReadFail());
        }

        /// Verified that all expected operations have now been completed.
        /// Panics if this is not true.
        pub fn expect_complete(&self) {
            if let Mode::Valid { operations, .. } = &self.mode {
                let ops = operations.try_lock().unwrap();
                if !ops.is_empty() {
                    panic!(
                        "FakeConnection has {:?} expected operations that were not performed",
                        ops.len()
                    );
                }
            }
        }
    }

    impl Drop for FakeConnection {
        fn drop(&mut self) {
            self.expect_complete();
        }
    }

    #[async_trait(?Send)]
    impl Connection for FakeConnection {
        async fn read_packet(&self) -> Result<Packet, Error> {
            match &self.mode {
                Mode::Valid { operations, .. } => match operations.lock().await.pop_front() {
                    Some(Operation::ReadSuccess(packet)) => Ok(packet),
                    Some(Operation::ReadFail()) => Err(format_err!("Read failing as requested")),
                    _ => panic!("Received unexpected read request"),
                },
                Mode::Invalid => Err(format_err!("Read called on set-to-fail fake connection")),
            }
        }

        async fn write_packet(&self, packet: Packet) -> Result<(), Error> {
            match &self.mode {
                Mode::Valid { operations, .. } => match operations.lock().await.pop_front() {
                    Some(Operation::WriteSuccess(expected)) => {
                        if packet != expected {
                            panic!(
                                "Received write request that did not match expectation:\n\
                                 Expected={:?}\nReceived={:?}",
                                expected, packet
                            );
                        }
                        Ok(())
                    }
                    Some(Operation::WriteFail(expected)) => {
                        if packet != expected {
                            panic!(
                                "Received write request that did not match expectation:\n\
                                 Expected={:?}\nReceived={:?}",
                                expected, packet
                            );
                        }
                        Err(format_err!("Write failing as requested"))
                    }
                    _ => panic!("Received unexpected write request"),
                },
                Mode::Invalid => Err(format_err!("Write called on set-to-fail fake connection")),
            }
        }

        async fn report_descriptor(&self) -> Result<Bytes, Error> {
            match &self.mode {
                Mode::Valid { report_desc, .. } => Ok(Bytes::clone(&report_desc)),
                Mode::Invalid => Err(format_err!("Method called on set-to-fail fake connection")),
            }
        }

        async fn max_packet_size(&self) -> Result<u16, Error> {
            match self.mode {
                Mode::Valid { .. } => Ok(REPORT_SIZE),
                Mode::Invalid => Err(format_err!("Method called on set-to-fail fake connection")),
            }
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use crate::hid::message::{Command, Packet};
        use fuchsia_async as fasync;
        use lazy_static::lazy_static;

        const TEST_REPORT_DESCRIPTOR: [u8; 8] = [0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01, 0x09];

        lazy_static! {
            static ref TEST_PACKET_1: Packet =
                Packet::initialization(0x12345678, Command::Init, 0, vec![]).unwrap();
            static ref TEST_PACKET_2: Packet =
                Packet::initialization(0x23456789, Command::Wink, 4, vec![0xff, 0xee, 0xdd, 0xcc])
                    .unwrap();
            static ref TEST_PACKET_3: Packet =
                Packet::continuation(0x34567890, 3, vec![0x99, 0x99]).unwrap();
        }

        #[fasync::run_until_stalled(test)]
        async fn test_static_properties() -> Result<(), Error> {
            let connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            assert_eq!(connection.report_descriptor().await?, &TEST_REPORT_DESCRIPTOR[..]);
            assert_eq!(connection.max_packet_size().await?, REPORT_SIZE);
            Ok(())
        }

        #[fasync::run_until_stalled(test)]
        async fn test_read_write() -> Result<(), Error> {
            // Declare expected operations.
            let connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            connection.expect_write_fail(TEST_PACKET_1.clone());
            connection.expect_write(TEST_PACKET_1.clone());
            connection.expect_read_fail();
            connection.expect_read(TEST_PACKET_2.clone());
            connection.expect_write(TEST_PACKET_3.clone());
            // Perform operations.
            connection
                .write_packet(TEST_PACKET_1.clone())
                .await
                .expect_err("Write should have failed");
            connection
                .write_packet(TEST_PACKET_1.clone())
                .await
                .expect("Write should have succeeded");
            connection.read_packet().await.expect_err("Read should have failed");
            assert_eq!(
                connection.read_packet().await.expect("Read should have succeeded"),
                *TEST_PACKET_2
            );
            connection
                .write_packet(TEST_PACKET_3.clone())
                .await
                .expect("Write should have succeeded");
            // Verify all expected operations occurred.
            connection.expect_complete();
            Ok(())
        }

        #[fasync::run_until_stalled(test)]
        #[should_panic]
        async fn test_write_unexpected_data() {
            let connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            connection.expect_write(TEST_PACKET_1.clone());
            connection.write_packet(TEST_PACKET_2.clone()).await.unwrap();
        }

        #[fasync::run_until_stalled(test)]
        #[should_panic]
        async fn test_write_when_expecting_read() {
            let connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            connection.expect_read(TEST_PACKET_1.clone());
            connection.write_packet(TEST_PACKET_1.clone()).await.unwrap();
        }

        #[fasync::run_until_stalled(test)]
        #[should_panic]
        async fn test_read_when_expecting_write() {
            let connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            connection.expect_write(TEST_PACKET_1.clone());
            connection.read_packet().await.unwrap();
        }

        #[fasync::run_until_stalled(test)]
        #[should_panic]
        async fn test_incomplete_operations() {
            let connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            connection.expect_write(TEST_PACKET_1.clone());
            // Dropping the connection should verify all expected operations are complete.
        }

        #[fasync::run_until_stalled(test)]
        async fn test_invalid() -> Result<(), Error> {
            let mut connection = FakeConnection::new(&TEST_REPORT_DESCRIPTOR);
            connection.report_descriptor().await.expect("Should have initially suceeded");
            connection.fail();
            connection.report_descriptor().await.expect_err("Should have failed to get descriptor");
            connection.max_packet_size().await.expect_err("Should have failed to get packet size");
            connection.read_packet().await.expect_err("Should have failed to read packet");
            connection
                .write_packet(TEST_PACKET_1.clone())
                .await
                .expect_err("Should have failed to write packet");
            Ok(())
        }
    }
}
