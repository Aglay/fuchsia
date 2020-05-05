// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use crate::prelude::*;

use fidl_fuchsia_lowpan_spinel::DeviceEvent as SpinelDeviceEvent;
use futures::channel::mpsc;
use futures::future::LocalBoxFuture;
use matches::assert_matches;

/// Returns a fake spinel device client and a future
/// to run in the background.
///
/// This is only used for testing.
pub fn new_fake_spinel_pair() -> (
    SpinelDeviceSink<MockDeviceProxy>,
    SpinelDeviceStream<MockDeviceProxy, mpsc::Receiver<Result<SpinelDeviceEvent, fidl::Error>>>,
    LocalBoxFuture<'static, ()>,
) {
    const INBOUND_WINDOW_SIZE: u32 = 2;
    let (device_sink, device_stream, mut device_event_sender, mut device_request_receiver) =
        new_mock_spinel_pair(2000);

    fn handle_get_prop(frame: SpinelFrameRef<'_>, prop: Prop) -> Option<Vec<u8>> {
        let mut response: Vec<u8> = vec![];
        match prop {
            Prop::ProtocolVersion => {
                spinel_write!(
                    &mut response,
                    "Ciiii",
                    frame.header,
                    Cmd::PropValueIs,
                    prop,
                    PROTOCOL_MAJOR_VERSION,
                    PROTOCOL_MINOR_VERSION
                )
                .unwrap();
            }
            Prop::NcpVersion => {
                spinel_write!(
                    &mut response,
                    "CiiU",
                    frame.header,
                    Cmd::PropValueIs,
                    prop,
                    "Dummy Device"
                )
                .unwrap();
            }
            Prop::Caps => {
                spinel_write!(
                    &mut response,
                    "Ciiii",
                    frame.header,
                    Cmd::PropValueIs,
                    prop,
                    CapConfig::Ftd,
                    CapNet::Thread(1, 1),
                )
                .unwrap();
            }
            Prop::Net(PropNet::Saved) => {
                spinel_write!(&mut response, "Ciib", frame.header, Cmd::PropValueIs, prop, false)
                    .unwrap();
            }
            Prop::Net(PropNet::InterfaceUp) => {
                spinel_write!(&mut response, "Ciib", frame.header, Cmd::PropValueIs, prop, false)
                    .unwrap();
            }
            Prop::Net(PropNet::StackUp) => {
                spinel_write!(&mut response, "Ciib", frame.header, Cmd::PropValueIs, prop, false)
                    .unwrap();
            }
            Prop::Phy(PropPhy::ChanSupported) => {
                spinel_write!(
                    &mut response,
                    "CiiCCC",
                    frame.header,
                    Cmd::PropValueIs,
                    prop,
                    11,
                    12,
                    13
                )
                .unwrap();
            }
            _ => {
                spinel_write!(
                    &mut response,
                    "Ciii",
                    frame.header,
                    Cmd::PropValueIs,
                    Prop::LastStatus,
                    Status::Unimplemented
                )
                .unwrap();
            }
        }
        Some(response)
    }

    fn handle_command(frame: &[u8]) -> Option<Vec<u8>> {
        let frame = SpinelFrameRef::try_unpack_from_slice(frame).unwrap();

        if frame.header.tid().is_none() {
            return None;
        }

        match frame.cmd {
            Cmd::PropValueGet => {
                let prop = Prop::try_unpack_from_slice(frame.payload).ok()?;
                handle_get_prop(frame, prop)
            }
            _ => {
                let mut response: Vec<u8> = vec![];
                spinel_write!(
                    &mut response,
                    "Ciii",
                    frame.header,
                    Cmd::PropValueIs,
                    Prop::LastStatus,
                    Status::Unimplemented
                )
                .unwrap();
                Some(response)
            }
        }
    }

    let ncp_task = async move {
        let mut outbound_frames_remaining = 0;
        let mut inbound_frames_remaining = 0;
        let mut is_open = false;
        let mut did_send_reset = false;

        traceln!("new_fake_spinel_device[ncp_task]: Start.");

        while let Some(request) = device_request_receiver.next().await {
            match request {
                DeviceRequest::Open => {
                    traceln!("new_fake_spinel_device[ncp_task]: Got Open");
                    is_open = true;

                    traceln!(
                        "new_fake_spinel_device[ncp_task]: Sending SpinelDeviceEvent::OnReadyForSendFrames"
                    );
                    let event = SpinelDeviceEvent::OnReadyForSendFrames {
                        number_of_frames: INBOUND_WINDOW_SIZE,
                    };
                    assert_matches!(device_event_sender.start_send(Ok(event)), Ok(_));
                    inbound_frames_remaining = INBOUND_WINDOW_SIZE;
                    outbound_frames_remaining = 0;
                    did_send_reset = false;
                }
                DeviceRequest::Close => {
                    traceln!("new_fake_spinel_device[ncp_task]: Got Close");
                    is_open = false;
                    outbound_frames_remaining = 0;
                }
                DeviceRequest::GetMaxFrameSize => {
                    traceln!("new_fake_spinel_device[ncp_task]: Got GetMaxFrameSize");
                }
                DeviceRequest::SendFrame(frame) => {
                    traceln!("new_fake_spinel_device[ncp_task]: Got SendFrame");
                    assert!(inbound_frames_remaining != 0, "inbound_frames_remaining == 0");
                    inbound_frames_remaining -= 1;

                    if frame == [129u8, 1u8] {
                        traceln!("new_fake_spinel_device[ncp_task]: Got software reset command!");
                        // reset!
                        did_send_reset = false;

                        if outbound_frames_remaining > 1 {
                            assert_matches!(
                                device_event_sender.start_send(Ok(
                                    SpinelDeviceEvent::OnReceiveFrame {
                                        data: vec![0x80, 0x06, 0x00, 113]
                                    }
                                )),
                                Ok(_)
                            );
                            did_send_reset = true;
                            outbound_frames_remaining -= 1;
                        }
                    } else if let Some(response) = handle_command(frame.as_slice()) {
                        assert!(
                            outbound_frames_remaining != 0,
                            "outbound_frames_remaining == 0, can't respond"
                        );
                        assert_matches!(
                            device_event_sender.start_send(Ok(SpinelDeviceEvent::OnReceiveFrame {
                                data: response
                            })),
                            Ok(_)
                        );
                        outbound_frames_remaining -= 1;
                    }

                    let event = SpinelDeviceEvent::OnReadyForSendFrames { number_of_frames: 1 };
                    assert_matches!(device_event_sender.start_send(Ok(event)), Ok(_));
                    inbound_frames_remaining += 1;
                }
                DeviceRequest::ReadyToReceiveFrames(n) => {
                    traceln!("new_fake_spinel_device[ncp_task]: Got ReadyToReceiveFrames");
                    assert!(is_open);
                    outbound_frames_remaining += n;
                    if !did_send_reset && outbound_frames_remaining > 1 {
                        assert_matches!(
                            device_event_sender.start_send(Ok(SpinelDeviceEvent::OnReceiveFrame {
                                data: vec![0x80, 0x06, 0x00, 113]
                            })),
                            Ok(_)
                        );
                        did_send_reset = true;
                        outbound_frames_remaining -= 1;
                    }
                }
            }
        }
    };

    (device_sink, device_stream, ncp_task.boxed_local())
}
