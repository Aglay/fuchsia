// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::component_control::ComponentController,
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_developer_remotecontrol as rcs, fidl_fuchsia_device as fdevice,
    fidl_fuchsia_device_manager as fdevmgr, fidl_fuchsia_net as fnet,
    fidl_fuchsia_net_stack as fnetstack, fidl_fuchsia_test_manager as ftest_manager,
    fuchsia_component::client::{launcher, AppBuilder},
    futures::prelude::*,
    std::rc::Rc,
};

mod component_control;

pub struct RemoteControlService {
    admin_proxy: fdevmgr::AdministratorProxy,
    netstack_proxy: fnetstack::StackProxy,
    harness_proxy: ftest_manager::HarnessProxy,
    name_provider_proxy: fdevice::NameProviderProxy,
}

impl RemoteControlService {
    pub fn new() -> Result<Self, Error> {
        let (admin_proxy, netstack_proxy, harness_proxy, name_provider_proxy) =
            Self::construct_proxies()?;
        return Ok(Self::new_with_proxies(
            admin_proxy,
            netstack_proxy,
            harness_proxy,
            name_provider_proxy,
        ));
    }

    pub fn new_with_proxies(
        admin_proxy: fdevmgr::AdministratorProxy,
        netstack_proxy: fnetstack::StackProxy,
        harness_proxy: ftest_manager::HarnessProxy,
        name_provider_proxy: fdevice::NameProviderProxy,
    ) -> Self {
        return Self { admin_proxy, netstack_proxy, harness_proxy, name_provider_proxy };
    }

    pub async fn serve_stream(
        self: Rc<Self>,
        mut stream: rcs::RemoteControlRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await.context("next RemoteControl request")? {
            match request {
                rcs::RemoteControlRequest::StartComponent {
                    component_url,
                    args,
                    component_stdout: stdout,
                    component_stderr: stderr,
                    controller,
                    responder,
                } => {
                    let mut response = self.clone().spawn_component_async(
                        &component_url,
                        args,
                        stdout,
                        stderr,
                        controller,
                    );
                    responder.send(&mut response).context("sending StartComponent response")?;
                }
                rcs::RemoteControlRequest::RebootDevice { reboot_type, responder } => {
                    self.clone().reboot_device(reboot_type, responder).await?;
                }
                rcs::RemoteControlRequest::IdentifyHost { responder } => {
                    self.clone().identify_host(responder).await?;
                }
                rcs::RemoteControlRequest::LaunchSuite {
                    test_url,
                    suite,
                    controller,
                    responder,
                } => {
                    log::info!("launching test suite {}", test_url);
                    let mut response = self
                        .harness_proxy
                        .launch_suite(&test_url, ftest_manager::LaunchOptions {}, suite, controller)
                        .await
                        .context("launch_test call failed")?;
                    responder.send(&mut response).context("sending LaunchSuite response")?;
                }
            }
        }
        Ok(())
    }

    fn construct_proxies() -> Result<
        (
            fdevmgr::AdministratorProxy,
            fnetstack::StackProxy,
            ftest_manager::HarnessProxy,
            fdevice::NameProviderProxy,
        ),
        Error,
    > {
        let admin_proxy =
            fuchsia_component::client::connect_to_service::<fdevmgr::AdministratorMarker>()
                .map_err(|s| format_err!("Failed to connect to DevMgr service: {}", s))?;
        let netstack_proxy =
            fuchsia_component::client::connect_to_service::<fnetstack::StackMarker>()
                .map_err(|s| format_err!("Failed to connect to NetStack service: {}", s))?;
        let harness_proxy =
            fuchsia_component::client::connect_to_service::<ftest_manager::HarnessMarker>()
                .map_err(|s| format_err!("Failed to connect to Harness service: {}", s))?;
        let name_provider_proxy =
            fuchsia_component::client::connect_to_service::<fdevice::NameProviderMarker>()
                .map_err(|s| format_err!("Failed to connect to NameProviderService: {}", s))?;
        return Ok((admin_proxy, netstack_proxy, harness_proxy, name_provider_proxy));
    }

    pub fn spawn_component_async(
        self: &Rc<Self>,
        component_name: &str,
        argv: Vec<String>,
        stdout: fidl::Socket,
        stderr: fidl::Socket,
        server_end: fidl::endpoints::ServerEnd<
            fidl_fuchsia_developer_remotecontrol::ComponentControllerMarker,
        >,
    ) -> Result<(), rcs::ComponentControlError> {
        log::info!("Attempting to start component '{}' with argv {:?}...", component_name, argv);
        let launcher = launcher().expect("Failed to open launcher service");
        let app = match AppBuilder::new(component_name)
            .stdout(stdout)
            .stderr(stderr)
            .args(argv)
            .spawn(&launcher)
        {
            Ok(app) => app,
            Err(e) => {
                log::error!("{}", e);
                return Err(rcs::ComponentControlError::ComponentControlFailure);
            }
        };

        let (stream, control_handle) = match server_end.into_stream_and_control_handle() {
            Ok((stream, control_handle)) => (stream, control_handle),
            Err(e) => {
                log::error!("{}", e);
                return Err(rcs::ComponentControlError::ControllerSetupFailure);
            }
        };
        let controller = ComponentController::new(app, stream, control_handle);

        hoist::spawn(async move {
            controller.serve().await.unwrap();
        });

        return Ok(());
    }

    pub async fn reboot_device(
        self: &Rc<Self>,
        reboot_type: rcs::RebootType,
        responder: rcs::RemoteControlRebootDeviceResponder,
    ) -> Result<(), Error> {
        responder.send().context("failed to send successful reboot response.")?;

        log::info!("got remote control request to reboot: {:?}", reboot_type);
        let suspend_flag = match reboot_type {
            rcs::RebootType::Reboot => fdevmgr::SUSPEND_FLAG_REBOOT,
            rcs::RebootType::Recovery => fdevmgr::SUSPEND_FLAG_REBOOT_RECOVERY,
            rcs::RebootType::Bootloader => fdevmgr::SUSPEND_FLAG_REBOOT_BOOTLOADER,
        };
        self.admin_proxy.suspend(suspend_flag).await?;
        Ok(())
    }

    pub async fn identify_host(
        self: &Rc<Self>,
        responder: rcs::RemoteControlIdentifyHostResponder,
    ) -> Result<(), Error> {
        let ilist = match self.netstack_proxy.list_interfaces().await {
            Ok(l) => l,
            Err(e) => {
                log::error!("Getting interface list failed: {}", e);
                responder
                    .send(&mut Err(rcs::IdentifyHostError::ListInterfacesFailed))
                    .context("sending IdentifyHost error response")?;
                return Ok(());
            }
        };

        let mut result: Vec<rcs::InterfaceAddress> = vec![];

        for int in ilist.iter() {
            for addr in int.properties.addresses.iter() {
                let iaddr = rcs::InterfaceAddress {
                    ip_address: match addr.ip_address {
                        fnet::IpAddress::Ipv4(i) => {
                            rcs::IpAddress::Ipv4(rcs::Ipv4Address { addr: i.addr })
                        }
                        fnet::IpAddress::Ipv6(i) => {
                            rcs::IpAddress::Ipv6(rcs::Ipv6Address { addr: i.addr })
                        }
                    },
                    prefix_len: addr.prefix_len,
                };
                result.push(iaddr);
            }
        }

        let nodename = match self.name_provider_proxy.get_device_name().await {
            Ok(result) => match result {
                Ok(name) => name,
                Err(e) => {
                    log::error!("NameProvider internal error: {}", e);
                    responder
                        .send(&mut Err(rcs::IdentifyHostError::GetDeviceNameFailed))
                        .context("sending GetDeviceName error response")?;
                    return Ok(());
                }
            },
            Err(e) => {
                log::error!("Getting nodename failed: {}", e);
                responder
                    .send(&mut Err(rcs::IdentifyHostError::GetDeviceNameFailed))
                    .context("sending GetDeviceName error response")?;
                return Ok(());
            }
        };

        responder
            .send(&mut Ok(rcs::IdentifyHostResponse {
                nodename: Some(nodename),
                addresses: Some(result),
            }))
            .context("sending IdentifyHost response")?;

        return Ok(());
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, fidl::endpoints::create_proxy, fidl_fuchsia_developer_remotecontrol as rcs,
        fidl_fuchsia_hardware_ethernet::MacAddress, fuchsia_async as fasync, fuchsia_zircon as zx,
    };

    const NODENAME: &'static str = "thumb-set-human-shred";

    // This is the exit code zircon will return when a component is killed.
    const EXIT_CODE_KILLED: i64 = -1024;
    // This is the exit code zircon will return for an non-existent package.
    const EXIT_CODE_START_FAILED: i64 = -1;

    const IPV4_ADDR: [u8; 4] = [127, 0, 0, 1];
    const IPV6_ADDR: [u8; 16] = [127, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 2, 3, 4, 5, 6];

    fn setup_fake_admin_service() -> fdevmgr::AdministratorProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fdevmgr::AdministratorMarker>().unwrap();

        fasync::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(fdevmgr::AdministratorRequest::Suspend {
                        flags: fdevmgr::SUSPEND_FLAG_REBOOT,
                        responder,
                    }) => {
                        let _ = responder.send(zx::Status::OK.into_raw());
                    }
                    Some(fdevmgr::AdministratorRequest::Suspend {
                        flags: fdevmgr::SUSPEND_FLAG_REBOOT_RECOVERY,
                        responder,
                    }) => {
                        let _ = responder.send(zx::Status::OK.into_raw());
                    }
                    Some(fdevmgr::AdministratorRequest::Suspend {
                        flags: fdevmgr::SUSPEND_FLAG_REBOOT_BOOTLOADER,
                        responder,
                    }) => {
                        let _ = responder.send(zx::Status::OK.into_raw());
                    }
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    fn setup_fake_name_provider_service() -> fdevice::NameProviderProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fdevice::NameProviderMarker>().unwrap();

        fasync::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(fdevice::NameProviderRequest::GetDeviceName { responder }) => {
                        let _ = responder.send(&mut Ok(String::from(NODENAME)));
                    }
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    fn setup_fake_harness_service() -> ftest_manager::HarnessProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<ftest_manager::HarnessMarker>().unwrap();

        fasync::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    fn setup_fake_netstack_service() -> fnetstack::StackProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fnetstack::StackMarker>().unwrap();

        fasync::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(fnetstack::StackRequest::ListInterfaces { responder }) => {
                        let mut resp = vec![fnetstack::InterfaceInfo {
                            id: 1,
                            properties: fnetstack::InterfaceProperties {
                                name: String::from("eth0"),
                                topopath: String::from("N/A"),
                                filepath: String::from("N/A"),
                                administrative_status: fnetstack::AdministrativeStatus::Enabled,
                                physical_status: fnetstack::PhysicalStatus::Up,
                                mtu: 1,
                                features: 0,
                                mac: Some(Box::new(MacAddress { octets: [1, 2, 3, 4, 5, 6] })),
                                addresses: vec![
                                    fnetstack::InterfaceAddress {
                                        ip_address: fnet::IpAddress::Ipv4(fnet::Ipv4Address {
                                            addr: IPV4_ADDR,
                                        }),
                                        prefix_len: 4,
                                    },
                                    fnetstack::InterfaceAddress {
                                        ip_address: fnet::IpAddress::Ipv6(fnet::Ipv6Address {
                                            addr: IPV6_ADDR,
                                        }),
                                        prefix_len: 6,
                                    },
                                ],
                            },
                        }];
                        let _ = responder.send(&mut resp.iter_mut());
                    }
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    async fn run_reboot_test(reboot_type: rcs::RebootType) {
        let rcs_proxy = setup_rcs();

        let response = rcs_proxy.reboot_device(reboot_type).await.unwrap();
        assert_eq!(response, ());
    }

    async fn verify_exit_code(proxy: rcs::ComponentControllerProxy, expected_exit_code: i64) {
        let events: Vec<_> = proxy.take_event_stream().collect::<Vec<_>>().await;

        assert_eq!(events.len(), 1);

        let event = events[0].as_ref().unwrap();
        match event {
            rcs::ComponentControllerEvent::OnTerminated { exit_code } => {
                assert_eq!(*exit_code, expected_exit_code);
            }
        };
    }

    fn verify_socket_content(s: fidl::Socket, expected: &str) {
        let mut value = Vec::new();

        let mut remaining = s.outstanding_read_bytes().or::<usize>(Ok(0usize)).unwrap();
        while remaining > 0 {
            let mut buf = [0u8; 128];
            let n = s.read(&mut buf).or::<usize>(Ok(0usize)).unwrap();
            value.extend_from_slice(&buf[..n]);
            remaining = s.outstanding_read_bytes().or::<usize>(Ok(0)).unwrap();
        }
        assert_eq!(std::str::from_utf8(&value).unwrap(), expected);
    }

    fn setup_rcs() -> rcs::RemoteControlProxy {
        let service: Rc<RemoteControlService>;
        service = Rc::new(RemoteControlService::new_with_proxies(
            setup_fake_admin_service(),
            setup_fake_netstack_service(),
            setup_fake_harness_service(),
            setup_fake_name_provider_service(),
        ));

        let (rcs_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<rcs::RemoteControlMarker>().unwrap();
        fasync::spawn_local(async move {
            service.serve_stream(stream).await.unwrap();
        });

        return rcs_proxy;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_spawn_hello_world() -> Result<(), Error> {
        let rcs_proxy = setup_rcs();
        let (proxy, server_end) = create_proxy::<rcs::ComponentControllerMarker>()?;
        let (sout, cout) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, cerr) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;

        let _ = rcs_proxy
            .start_component(
                "fuchsia-pkg://fuchsia.com/hello_world_rust#meta/hello_world_rust.cmx",
                &mut std::iter::empty::<_>(),
                sout,
                serr,
                server_end,
            )
            .await
            .unwrap()
            .unwrap();

        verify_exit_code(proxy, 0).await;
        verify_socket_content(cout, "Hello, world!\n");
        verify_socket_content(cerr, "");

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_spawn_and_kill() -> Result<(), Error> {
        let rcs_proxy = setup_rcs();
        let (proxy, server_end) = create_proxy::<rcs::ComponentControllerMarker>()?;
        let (sout, cout) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, cerr) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;

        let _ = rcs_proxy
            .start_component(
                "fuchsia-pkg://fuchsia.com/echo_server#meta/echo_server.cmx",
                &mut std::iter::empty::<_>(),
                sout,
                serr,
                server_end,
            )
            .and_then(|_| proxy.kill())
            .await?;

        verify_exit_code(proxy, EXIT_CODE_KILLED).await;
        verify_socket_content(cout, "");
        verify_socket_content(cerr, "");
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_start_non_existent_package() -> Result<(), Error> {
        let rcs_proxy = setup_rcs();
        let (proxy, server_end) = create_proxy::<rcs::ComponentControllerMarker>()?;
        let (sout, cout) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, cerr) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;

        let _start_response = rcs_proxy
            .start_component(
                "fuchsia-pkg://fuchsia.com/hello_world_rust#meta/this_package_doesnt_exist.cmx",
                &mut std::iter::empty::<_>(),
                sout,
                serr,
                server_end,
            )
            .await
            .unwrap()
            .unwrap();

        verify_exit_code(proxy, EXIT_CODE_START_FAILED).await;
        verify_socket_content(cout, "");
        verify_socket_content(cerr, "");
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_reboot() -> Result<(), Error> {
        run_reboot_test(rcs::RebootType::Reboot).await;
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_reboot_recovery() -> Result<(), Error> {
        run_reboot_test(rcs::RebootType::Recovery).await;
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_reboot_bootloader() -> Result<(), Error> {
        run_reboot_test(rcs::RebootType::Bootloader).await;
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_identify_host() -> Result<(), Error> {
        let rcs_proxy = setup_rcs();

        let resp = rcs_proxy.identify_host().await.unwrap().unwrap();

        assert_eq!(resp.nodename.unwrap(), NODENAME);

        let addrs = resp.addresses.unwrap();
        assert_eq!(addrs.len(), 2);

        let v4 = addrs[0];
        assert_eq!(v4.prefix_len, 4);
        assert_eq!(v4.ip_address, rcs::IpAddress::Ipv4(rcs::Ipv4Address { addr: IPV4_ADDR }));

        let v6 = addrs[1];
        assert_eq!(v6.prefix_len, 6);
        assert_eq!(v6.ip_address, rcs::IpAddress::Ipv6(rcs::Ipv6Address { addr: IPV6_ADDR }));

        Ok(())
    }
}
