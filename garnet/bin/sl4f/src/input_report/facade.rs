// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    common_utils::common::macros::{fx_err_and_bail, with_line},
    input_report::types::{
        InputDeviceMatchArgs, SerializableDeviceDescriptor, SerializableInputReport,
    },
};
use anyhow::Error;
use fidl_fuchsia_input_report::{InputDeviceMarker, InputDeviceProxy, InputReport};
use fuchsia_syslog::macros::*;
use glob::glob;
use parking_lot::{RwLock, RwLockUpgradableReadGuard};
use std::vec::Vec;

#[derive(Debug)]
pub struct InputReportFacade {
    proxy: RwLock<Option<InputDeviceProxy>>,
}

fn connect_to_device(path: std::path::PathBuf) -> Option<InputDeviceProxy> {
    match fidl::endpoints::create_proxy::<InputDeviceMarker>() {
        Ok((proxy, server)) => {
            match fdio::service_connect(&path.to_string_lossy(), server.into_channel()) {
                Ok(_r) => Some(proxy),
                Err(_e) => None,
            }
        }
        Err(_e) => None,
    }
}

async fn check_device_match(
    proxy: InputDeviceProxy,
    match_args: &InputDeviceMatchArgs,
) -> Option<InputDeviceProxy> {
    match proxy.get_descriptor().await.ok().map(|desc| desc.device_info).flatten() {
        Some(info) => {
            // Accept the device if all specified match arguments are equal to the corresponding
            // fields in DeviceInfo.
            if match_args.vendor_id.unwrap_or(info.vendor_id) != info.vendor_id {
                None
            } else if match_args.product_id.unwrap_or(info.product_id) != info.product_id {
                None
            } else if match_args.version.unwrap_or(info.version) != info.version {
                None
            } else {
                Some(proxy)
            }
        }
        None => match_args
            .vendor_id
            .or(match_args.product_id)
            .or(match_args.version)
            // The device doesn't provide DeviceInfo -- only accept if if no match arguments were
            // specified.
            .map_or(Some(proxy), |_num| None),
    }
}

impl InputReportFacade {
    pub fn new() -> InputReportFacade {
        Self { proxy: RwLock::new(None) }
    }

    #[cfg(test)]
    fn new_with_proxy(proxy: InputDeviceProxy) -> Self {
        Self { proxy: RwLock::new(Some(proxy)) }
    }

    async fn get_proxy(&self, match_args: InputDeviceMatchArgs) -> Result<InputDeviceProxy, Error> {
        let lock = self.proxy.upgradable_read();
        if let Some(proxy) = lock.as_ref() {
            Ok(proxy.clone())
        } else {
            let tag = "InputReportFacade::get_proxy";

            let mut devices = Vec::<InputDeviceProxy>::new();
            for proxy in glob("/dev/class/input-report/*")?
                .filter_map(Result::ok)
                .filter_map(connect_to_device)
            {
                if let Some(p) = check_device_match(proxy, &match_args).await {
                    devices.push(p);
                }
            }

            if devices.len() < 1 {
                fx_err_and_bail!(&with_line!(tag), "Failed to find matching input report device")
            } else if devices.len() > 1 {
                fx_err_and_bail!(&with_line!(tag), "Found multiple matching input report devices")
            } else {
                let proxy = devices.remove(0);
                *RwLockUpgradableReadGuard::upgrade(lock) = Some(proxy.clone());
                Ok(proxy)
            }
        }
    }

    pub async fn get_reports(
        &self,
        match_args: InputDeviceMatchArgs,
    ) -> Result<Vec<SerializableInputReport>, Error> {
        let reports: Vec<InputReport> = self.get_proxy(match_args).await?.get_reports().await?;
        let mut serializable_reports = Vec::<SerializableInputReport>::new();
        for report in reports {
            serializable_reports.push(SerializableInputReport::new(&report));
        }
        Ok(serializable_reports)
    }

    pub async fn get_descriptor(
        &self,
        match_args: InputDeviceMatchArgs,
    ) -> Result<SerializableDeviceDescriptor, Error> {
        let descriptor = self.get_proxy(match_args).await?.get_descriptor().await?;
        Ok(SerializableDeviceDescriptor::new(&descriptor))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input_report::types::*;
    use fidl_fuchsia_input_report::*;
    use futures::{future::Future, join, stream::StreamExt};
    use matches::assert_matches;

    struct MockInputReportBuilder {
        expected: Vec<Box<dyn FnOnce(InputDeviceRequest) + Send + 'static>>,
    }

    impl MockInputReportBuilder {
        fn new() -> Self {
            Self { expected: vec![] }
        }

        fn push(mut self, request: impl FnOnce(InputDeviceRequest) + Send + 'static) -> Self {
            self.expected.push(Box::new(request));
            self
        }

        fn expect_get_reports(self, reports: Vec<InputReport>) -> Self {
            self.push(move |req| match req {
                InputDeviceRequest::GetReports { responder } => {
                    assert_matches!(responder.send(&mut reports.into_iter()), Ok(()));
                }
                req => panic!("unexpected request: {:?}", req),
            })
        }

        fn expect_get_descriptor(self, descriptor: DeviceDescriptor) -> Self {
            self.push(move |req| match req {
                InputDeviceRequest::GetDescriptor { responder } => {
                    assert_matches!(responder.send(descriptor), Ok(()));
                }
                req => panic!("unexpected request: {:?}", req),
            })
        }

        fn build(self) -> (InputReportFacade, impl Future<Output = ()>) {
            let (proxy, mut stream) =
                fidl::endpoints::create_proxy_and_stream::<InputDeviceMarker>().unwrap();
            let fut = async move {
                for expected in self.expected {
                    expected(stream.next().await.unwrap().unwrap());
                }
                assert_matches!(stream.next().await, None);
            };

            (InputReportFacade::new_with_proxy(proxy), fut)
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_device_descriptor() {
        let (facade, expectations) = MockInputReportBuilder::new()
            .expect_get_descriptor(DeviceDescriptor {
                device_info: Some(DeviceInfo {
                    vendor_id: 1,
                    product_id: 2,
                    version: 3,
                    name: "abcd".to_string(),
                }),
                mouse: None,
                sensor: Some(SensorDescriptor {
                    input: Some(SensorInputDescriptor {
                        values: Some(vec![
                            SensorAxis {
                                axis: Axis {
                                    range: Range { min: -100, max: 100 },
                                    unit: Unit::Acceleration,
                                },
                                type_: SensorType::AccelerometerX,
                            },
                            SensorAxis {
                                axis: Axis {
                                    range: Range { min: -10000, max: 10000 },
                                    unit: Unit::MagneticFlux,
                                },
                                type_: SensorType::MagnetometerX,
                            },
                            SensorAxis {
                                axis: Axis {
                                    range: Range { min: 0, max: 1000 },
                                    unit: Unit::LuminousFlux,
                                },
                                type_: SensorType::LightIlluminance,
                            },
                        ]),
                    }),
                }),
                touch: Some(TouchDescriptor {
                    input: Some(TouchInputDescriptor {
                        contacts: Some(vec![
                            ContactInputDescriptor {
                                position_x: Some(Axis {
                                    range: Range { min: 0, max: 200 },
                                    unit: Unit::Distance,
                                }),
                                position_y: Some(Axis {
                                    range: Range { min: 0, max: 100 },
                                    unit: Unit::Distance,
                                }),
                                pressure: Some(Axis {
                                    range: Range { min: 0, max: 10 },
                                    unit: Unit::Pressure,
                                }),
                                contact_width: None,
                                contact_height: None,
                            },
                            ContactInputDescriptor {
                                position_x: Some(Axis {
                                    range: Range { min: 0, max: 200 },
                                    unit: Unit::Distance,
                                }),
                                position_y: Some(Axis {
                                    range: Range { min: 0, max: 100 },
                                    unit: Unit::Distance,
                                }),
                                pressure: Some(Axis {
                                    range: Range { min: 0, max: 10 },
                                    unit: Unit::Pressure,
                                }),
                                contact_width: None,
                                contact_height: None,
                            },
                        ]),
                        max_contacts: Some(2),
                        touch_type: Some(TouchType::Touchpad),
                        buttons: Some(vec![1, 2, 3]),
                    }),
                }),
                keyboard: None,
                consumer_control: None,
            })
            .build();

        let test = async move {
            let descriptor = facade.get_descriptor(InputDeviceMatchArgs::default()).await;
            assert!(descriptor.is_ok());
            assert_eq!(
                descriptor.unwrap(),
                SerializableDeviceDescriptor {
                    device_info: Some(SerializableDeviceInfo {
                        vendor_id: 1,
                        product_id: 2,
                        version: 3,
                        name: "abcd".to_string(),
                    }),
                    sensor: Some(SerializableSensorDescriptor {
                        input: Some(SerializableSensorInputDescriptor {
                            values: Some(vec![
                                SerializableSensorAxis {
                                    axis: SerializableAxis {
                                        range: SerializableRange { min: -100, max: 100 },
                                        unit: Unit::Acceleration.into_primitive(),
                                    },
                                    type_: SensorType::AccelerometerX.into_primitive(),
                                },
                                SerializableSensorAxis {
                                    axis: SerializableAxis {
                                        range: SerializableRange { min: -10000, max: 10000 },
                                        unit: Unit::MagneticFlux.into_primitive(),
                                    },
                                    type_: SensorType::MagnetometerX.into_primitive(),
                                },
                                SerializableSensorAxis {
                                    axis: SerializableAxis {
                                        range: SerializableRange { min: 0, max: 1000 },
                                        unit: Unit::LuminousFlux.into_primitive(),
                                    },
                                    type_: SensorType::LightIlluminance.into_primitive(),
                                },
                            ])
                        })
                    }),
                    touch: Some(SerializableTouchDescriptor {
                        input: Some(SerializableTouchInputDescriptor {
                            contacts: Some(vec![
                                SerializableContactInputDescriptor {
                                    position_x: Some(SerializableAxis {
                                        range: SerializableRange { min: 0, max: 200 },
                                        unit: Unit::Distance.into_primitive(),
                                    }),
                                    position_y: Some(SerializableAxis {
                                        range: SerializableRange { min: 0, max: 100 },
                                        unit: Unit::Distance.into_primitive(),
                                    }),
                                    pressure: Some(SerializableAxis {
                                        range: SerializableRange { min: 0, max: 10 },
                                        unit: Unit::Pressure.into_primitive(),
                                    }),
                                    contact_width: None,
                                    contact_height: None,
                                },
                                SerializableContactInputDescriptor {
                                    position_x: Some(SerializableAxis {
                                        range: SerializableRange { min: 0, max: 200 },
                                        unit: Unit::Distance.into_primitive(),
                                    }),
                                    position_y: Some(SerializableAxis {
                                        range: SerializableRange { min: 0, max: 100 },
                                        unit: Unit::Distance.into_primitive(),
                                    }),
                                    pressure: Some(SerializableAxis {
                                        range: SerializableRange { min: 0, max: 10 },
                                        unit: Unit::Pressure.into_primitive(),
                                    }),
                                    contact_width: None,
                                    contact_height: None,
                                },
                            ]),
                            max_contacts: Some(2),
                            touch_type: Some(TouchType::Touchpad.into_primitive()),
                            buttons: Some(vec![1, 2, 3]),
                        })
                    }),
                }
            );
        };

        join!(expectations, test);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_reports() {
        let (facade, expectations) = MockInputReportBuilder::new()
            .expect_get_reports(vec![
                InputReport {
                    event_time: None,
                    mouse: None,
                    sensor: Some(SensorInputReport { values: Some(vec![1, 2, 3, 4, 5]) }),
                    touch: Some(TouchInputReport {
                        contacts: Some(vec![
                            ContactInputReport {
                                contact_id: Some(1),
                                position_x: Some(100),
                                position_y: Some(200),
                                pressure: Some(10),
                                contact_width: None,
                                contact_height: None,
                            },
                            ContactInputReport {
                                contact_id: Some(2),
                                position_x: Some(20),
                                position_y: Some(10),
                                pressure: Some(5),
                                contact_width: None,
                                contact_height: None,
                            },
                            ContactInputReport {
                                contact_id: Some(3),
                                position_x: Some(0),
                                position_y: Some(0),
                                pressure: Some(1),
                                contact_width: None,
                                contact_height: None,
                            },
                        ]),
                        pressed_buttons: Some(vec![4, 5, 6]),
                    }),
                    keyboard: None,
                    consumer_control: None,
                    trace_id: Some(1),
                },
                InputReport {
                    event_time: Some(1000),
                    mouse: None,
                    sensor: Some(SensorInputReport { values: Some(vec![6, 7, 8, 9, 10]) }),
                    touch: None,
                    keyboard: None,
                    consumer_control: None,
                    trace_id: Some(2),
                },
                InputReport {
                    event_time: Some(2000),
                    mouse: None,
                    sensor: None,
                    touch: Some(TouchInputReport {
                        contacts: Some(vec![
                            ContactInputReport {
                                contact_id: Some(1),
                                position_x: Some(1000),
                                position_y: Some(2000),
                                pressure: Some(5),
                                contact_width: None,
                                contact_height: None,
                            },
                            ContactInputReport {
                                contact_id: Some(3),
                                position_x: Some(10),
                                position_y: Some(10),
                                pressure: Some(5),
                                contact_width: None,
                                contact_height: None,
                            },
                        ]),
                        pressed_buttons: Some(vec![1, 2, 3]),
                    }),
                    keyboard: None,
                    consumer_control: None,
                    trace_id: Some(3),
                },
            ])
            .build();

        let test = async move {
            let report = facade.get_reports(InputDeviceMatchArgs::default()).await;
            assert!(report.is_ok());
            assert_eq!(
                report.unwrap(),
                vec![
                    SerializableInputReport {
                        event_time: None,
                        sensor: Some(SerializableSensorInputReport {
                            values: Some(vec![1, 2, 3, 4, 5])
                        }),
                        touch: Some(SerializableTouchInputReport {
                            contacts: Some(vec![
                                SerializableContactInputReport {
                                    contact_id: Some(1),
                                    position_x: Some(100),
                                    position_y: Some(200),
                                    pressure: Some(10),
                                    contact_width: None,
                                    contact_height: None,
                                },
                                SerializableContactInputReport {
                                    contact_id: Some(2),
                                    position_x: Some(20),
                                    position_y: Some(10),
                                    pressure: Some(5),
                                    contact_width: None,
                                    contact_height: None,
                                },
                                SerializableContactInputReport {
                                    contact_id: Some(3),
                                    position_x: Some(0),
                                    position_y: Some(0),
                                    pressure: Some(1),
                                    contact_width: None,
                                    contact_height: None,
                                },
                            ]),
                            pressed_buttons: Some(vec![4, 5, 6]),
                        }),
                        trace_id: Some(1),
                    },
                    SerializableInputReport {
                        event_time: Some(1000),
                        sensor: Some(SerializableSensorInputReport {
                            values: Some(vec![6, 7, 8, 9, 10])
                        }),
                        touch: None,
                        trace_id: Some(2),
                    },
                    SerializableInputReport {
                        event_time: Some(2000),
                        sensor: None,
                        touch: Some(SerializableTouchInputReport {
                            contacts: Some(vec![
                                SerializableContactInputReport {
                                    contact_id: Some(1),
                                    position_x: Some(1000),
                                    position_y: Some(2000),
                                    pressure: Some(5),
                                    contact_width: None,
                                    contact_height: None,
                                },
                                SerializableContactInputReport {
                                    contact_id: Some(3),
                                    position_x: Some(10),
                                    position_y: Some(10),
                                    pressure: Some(5),
                                    contact_width: None,
                                    contact_height: None,
                                },
                            ]),
                            pressed_buttons: Some(vec![1, 2, 3]),
                        }),
                        trace_id: Some(3),
                    },
                ]
            );
        };

        join!(expectations, test);
    }
}
