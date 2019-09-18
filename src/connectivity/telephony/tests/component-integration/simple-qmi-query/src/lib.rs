// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{Error, ResultExt},
    fidl_fuchsia_device::ControllerSynchronousProxy,
    fidl_fuchsia_io,
    fidl_fuchsia_telephony_ril::{RadioInterfaceLayerMarker, SetupMarker},
    fuchsia_component::{
        client::{launch, launcher},
        fuchsia_single_component_package_url,
    },
    fuchsia_syslog::{self as syslog, macros::*},
    fuchsia_zircon as zx, qmi,
    std::fs::File,
    std::path::Path,
    tel_dev::isolated_devmgr,
};

const RIL_URL: &str = fuchsia_single_component_package_url!("ril-qmi");

fn unbind_fake_device(device: &File) -> Result<(), Error> {
    let channel = fdio::clone_channel(device)?;
    let mut interface = ControllerSynchronousProxy::new(channel);
    let status = interface.schedule_unbind(fuchsia_zircon::Time::INFINITE)?;
    zx::Status::ok(status)?;
    Ok(())
}

// Tests that creating and destroying a fake QMI device binds and unbinds the qmi-host driver.
#[fuchsia_async::run_singlethreaded(test)]
async fn qmi_query_test() -> Result<(), Error> {
    syslog::init_with_tags(&["qmi-query-test"]).expect("Can't init logger");
    const TEL_PATH: &str = "class/qmi-transport";
    let tel_dir =
        isolated_devmgr::open_dir_in_isolated_devmgr(TEL_PATH).expect("opening qmi-transport dir");
    let mut directory_proxy = fidl_fuchsia_io::DirectoryProxy::new(
        fuchsia_async::Channel::from_channel(fdio::clone_channel(&tel_dir)?)?,
    );
    let mut tel_devices = files_async::readdir(&directory_proxy).await?;
    let last_device: &files_async::DirEntry = tel_devices.last().expect("no device found");
    let found_device_path = Box::new(Path::new(TEL_PATH).join(last_device.name.clone()));
    assert_eq!(tel_devices.len(), 1);
    fx_log_info!("connecting to {:?}", &*found_device_path);
    let found_device = isolated_devmgr::open_file_in_isolated_devmgr(*found_device_path).unwrap();
    let launcher = launcher().context("Failed to open launcher service")?;
    let chan = qmi::connect_transport_device(&found_device).await?;
    let app =
        launch(&launcher, RIL_URL.to_string(), None).context("Failed to launch ril-qmi service")?;
    let ril_modem_setup = app.connect_to_service::<SetupMarker>()?;
    ril_modem_setup.connect_transport(chan).await?.expect("make sure telephony svc is running");
    let ril_modem = app.connect_to_service::<RadioInterfaceLayerMarker>()?;
    fx_log_info!("sending a IMEI request");
    let imei = ril_modem.get_device_identity().await?.expect("error sending IMEI request");
    assert_eq!(imei, "359260080168351");
    let imei = ril_modem.get_device_identity().await?.expect("error sending IMEI request");
    assert_eq!(imei, "359260080168351");
    fx_log_info!("received and verified responses");
    unbind_fake_device(&found_device)?;
    fx_log_info!("unbinded device");
    directory_proxy = fidl_fuchsia_io::DirectoryProxy::new(fuchsia_async::Channel::from_channel(
        fdio::clone_channel(&tel_dir)?,
    )?);
    fx_log_info!("dirproxy created");
    tel_devices = files_async::readdir(&directory_proxy).await?;
    assert_eq!(tel_devices.len(), 0);
    Ok(())
}
