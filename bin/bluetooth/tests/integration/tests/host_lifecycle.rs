// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

extern crate failure;
extern crate fdio;
extern crate fidl_fuchsia_bluetooth_host;
extern crate fuchsia_async as async;
extern crate fuchsia_bluetooth;
extern crate futures;
extern crate rand;

use failure::Error;
use fidl_fuchsia_bluetooth_host::HostProxy;
use fuchsia_bluetooth::fake_hci::FakeHciDevice;
use fuchsia_bluetooth::hci;
use fuchsia_bluetooth::host;
use std::path::PathBuf;
use std::{thread, time};

mod common;

// The maximum amount of time spent polling
const MAX_POLL_MS: u64 = 30000;
const SLEEP_MS: u64 = 500;
const ITERATIONS: u64 = MAX_POLL_MS / SLEEP_MS;

fn sleep() -> () {
    thread::sleep(time::Duration::from_millis(SLEEP_MS));
}

// Tests that creating and destroying a fake HCI device binds and unbinds the bt-host driver.
fn lifecycle_test() -> Result<(), Error> {
    let original_hosts = host::list_host_devices();
    let fake_hci = FakeHciDevice::new()?;

    // TODO(armansito): Use a device watcher instead of polling.

    let mut bthost = PathBuf::from("");
    let mut retry = 0;
    'find_device: while retry < ITERATIONS {
        retry += 1;
        let new_hosts = host::list_host_devices();
        for host in new_hosts {
            if !original_hosts.contains(&host) {
                bthost = host;
                break 'find_device;
            }
        }
        sleep();
    }

    // Check a device showed up within an acceptable timeout
    let found_device = common::open_rdwr(&bthost);
    assert!(found_device.is_ok());
    let found_device = found_device.unwrap();

    // Check the right driver is bound to the device
    let driver_name = hci::get_device_driver_name(&found_device).unwrap();
    assert_eq!("bthost", driver_name.to_str().unwrap());

    // Confirm device topology, host is under bt-hci
    let device_topo = fdio::device_get_topo_path(&found_device).unwrap();
    assert!(device_topo.contains("bt-hci"));

    // Open a host channel using an ioctl and check the device is responsive
    let mut executor = async::Executor::new().unwrap();
    let handle = host::open_host_channel(&found_device).unwrap();
    let host = HostProxy::new(async::Channel::from_channel(handle.into()).unwrap());
    let info = executor.run_singlethreaded(host.get_info());
    assert!(info.is_ok());
    if let Ok(info) = info {
        assert_eq!("00:00:00:00:00:00", info.address);
    }

    // Remove the bt-hci device
    drop(fake_hci);

    // Check the host driver is also destroyed
    let _post_destroy_hosts = host::list_host_devices();
    let mut device_found = true;
    let mut retry = 0;
    while retry < ITERATIONS {
        retry += 1;
        let new_hosts = host::list_host_devices();
        if !new_hosts.contains(&bthost) {
            device_found = false;
            break;
        }
        sleep();
    }
    assert!(!device_found);

    Ok(())
}

fn main() -> Result<(), Error> {
    lifecycle_test()
}
