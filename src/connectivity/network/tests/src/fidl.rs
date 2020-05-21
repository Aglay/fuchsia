// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_net_stack_ext::{exec_fidl, FidlReturn};
use net_declare::fidl_ip;

use anyhow::Context as _;

use crate::environments::*;
use crate::Result;
use futures::TryStreamExt as _;

/// Regression test: test that Netstack.SetInterfaceStatus does not kill the channel to the client
/// if given an invalid interface id.
#[fuchsia_async::run_singlethreaded(test)]
async fn set_interface_status_unknown_interface() -> Result {
    let name = "set_interface_status";
    let sandbox = TestSandbox::new()?;
    let (_env, netstack) =
        sandbox.new_netstack::<Netstack2, fidl_fuchsia_netstack::NetstackMarker>(name)?;

    let interfaces = netstack.get_interfaces2().await.context("failed to call get_interfaces2")?;
    let next_id =
        1 + interfaces.iter().map(|interface| interface.id).max().ok_or(anyhow::format_err!(
            "failed to find any network interfaces (at least localhost should be present)"
        ))?;

    let () = netstack
        .set_interface_status(next_id, false)
        .context("failed to call set_interface_status")?;
    let _interfaces = netstack
        .get_interfaces2()
        .await
        .context("failed to invoke netstack method after calling set_interface_status with an invalid argument")?;

    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn inspect_objects() -> Result {
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let env = sandbox
        .create_empty_environment("inspect_objects")
        .context("failed to create environment")?;
    let launcher = env.get_launcher().context("failed to create launcher")?;

    let netstack = fuchsia_component::client::launch(
        &launcher,
        <Netstack2 as Netstack>::VERSION.get_url().to_string(),
        None,
    )
    .context("failed to start netstack")?;

    // TODO(fxbug.dev/4629): the launcher API lies and claims it connects you to "the" directory
    // request, but it doesn't. It connects you to the "svc" directory under the directory request.
    // That means that reading anything other than FIDL services isn't possible, and THAT means
    // that this test is impossible.
    if false {
        for path in ["counters", "interfaces"].iter() {
            let (client, server) =
                fidl::endpoints::create_proxy::<fidl_fuchsia_inspect_deprecated::InspectMarker>()
                    .context("failed to create proxy")?;

            let path = format!("diagnostics/{}/inspect", path);
            let () = netstack
                .pass_to_named_service(&path, server.into_channel())
                .with_context(|| format!("failed to connect to {}", path))?;

            let _object = client.read_data().await.context("failed to call ReadData")?;
        }
    }
    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn add_ethernet_device() -> Result {
    let name = "add_ethernet_device";
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (_env, netstack, device) = sandbox
        .new_netstack_and_device::<Netstack2, fidl_fuchsia_netstack::NetstackMarker>(name)
        .await?;

    let id = netstack
        .add_ethernet_device(
            name,
            &mut fidl_fuchsia_netstack::InterfaceConfig {
                name: name[..fidl_fuchsia_posix_socket::INTERFACE_NAME_LENGTH.into()].to_string(),
                filepath: "/fake/filepath/for_test".to_string(),
                metric: 0,
                ip_address_config: fidl_fuchsia_netstack::IpAddressConfig::Dhcp(true),
            },
            // We're testing add_ethernet_device (netstack.fidl), which
            // does not have a network device entry point.
            device
                .get_ethernet()
                .await
                .context("add_ethernet_device requires an Ethernet endpoint")?,
        )
        .await
        .context("failed to add ethernet device")?;
    let interface = netstack
        .get_interfaces2()
        .await
        .context("failed to get interfaces")?
        .into_iter()
        .find(|interface| interface.id == id)
        .ok_or(anyhow::format_err!("failed to find added ethernet device"))?;
    assert_eq!(interface.features & fidl_fuchsia_hardware_ethernet::INFO_FEATURE_LOOPBACK, 0);
    assert_eq!(interface.flags & fidl_fuchsia_netstack::NET_INTERFACE_FLAG_UP, 0);
    Ok::<(), anyhow::Error>(())
}

async fn add_ethernet_interface<N: Netstack>(name: &'static str) -> Result {
    let sandbox = TestSandbox::new()?;
    let (_env, stack, device) =
        sandbox.new_netstack_and_device::<N, fidl_fuchsia_net_stack::StackMarker>(name).await?;
    let id = device.add_to_stack(&stack).await?;
    let interface = stack
        .list_interfaces()
        .await
        .context("failed to list interfaces")?
        .into_iter()
        .find(|interface| interface.id == id)
        .ok_or(anyhow::format_err!("failed to find added ethernet interface"))?;
    assert_eq!(
        interface.properties.features & fidl_fuchsia_hardware_ethernet::INFO_FEATURE_LOOPBACK,
        0
    );
    assert_eq!(interface.properties.physical_status, fidl_fuchsia_net_stack::PhysicalStatus::Down);
    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn add_ethernet_interface_n2() -> Result {
    add_ethernet_interface::<Netstack2>("add_ethernet_interface_n2").await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn add_ethernet_interface_n3() -> Result {
    add_ethernet_interface::<Netstack3>("add_ethernet_interface_n3").await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn add_del_interface_address() -> Result {
    let name = "add_del_interface_address";

    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (_env, stack) = sandbox
        .new_netstack::<Netstack2, fidl_fuchsia_net_stack::StackMarker>(name)
        .context("failed to create environment")?;

    let interfaces = stack.list_interfaces().await.context("failed to list interfaces")?;
    let loopback = interfaces
        .iter()
        .find(|interface| {
            interface.properties.features & fidl_fuchsia_hardware_ethernet::INFO_FEATURE_LOOPBACK
                != 0
        })
        .ok_or(anyhow::format_err!("failed to find loopback"))?;
    let mut interface_address =
        fidl_fuchsia_net_stack::InterfaceAddress { ip_address: fidl_ip!(1.1.1.1), prefix_len: 32 };
    let res = stack
        .add_interface_address(loopback.id, &mut interface_address)
        .await
        .context("failed to call add interface address")?;
    assert_eq!(res, Ok(()));
    let loopback =
        exec_fidl!(stack.get_interface_info(loopback.id), "failed to get loopback interface")?;

    assert!(
        loopback.properties.addresses.iter().find(|addr| *addr == &interface_address).is_some(),
        "couldn't find {:#?} in {:#?}",
        interface_address,
        loopback.properties.addresses
    );

    let res = stack
        .del_interface_address(loopback.id, &mut interface_address)
        .await
        .context("failed to call del interface address")?;
    assert_eq!(res, Ok(()));
    let loopback =
        exec_fidl!(stack.get_interface_info(loopback.id), "failed to get loopback interface")?;

    assert!(
        loopback.properties.addresses.iter().find(|addr| *addr == &interface_address).is_none(),
        "did not expect to find {:#?} in {:#?}",
        interface_address,
        loopback.properties.addresses
    );

    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn add_remove_interface_address_errors() -> Result {
    let name = "add_remove_interface_address_errors";

    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (env, stack) = sandbox
        .new_netstack::<Netstack2, fidl_fuchsia_net_stack::StackMarker>(name)
        .context("failed to create environment")?;
    let netstack = env
        .connect_to_service::<fidl_fuchsia_netstack::NetstackMarker>()
        .context("failed to connect to netstack")?;

    let interfaces = stack.list_interfaces().await.context("failed to list interfaces")?;
    let max_id = interfaces.iter().map(|interface| interface.id).max().unwrap_or(0);
    let mut interface_address =
        fidl_fuchsia_net_stack::InterfaceAddress { ip_address: fidl_ip!(0.0.0.0), prefix_len: 0 };

    // Don't crash on interface not found.

    let error = stack
        .add_interface_address(max_id + 1, &mut interface_address)
        .await
        .context("failed to call add interface address")?
        .unwrap_err();
    assert_eq!(error, fidl_fuchsia_net_stack::Error::NotFound);

    let error = netstack
        .remove_interface_address(
            std::convert::TryInto::try_into(max_id + 1).expect("should fit"),
            &mut interface_address.ip_address,
            interface_address.prefix_len,
        )
        .await
        .context("failed to call add interface address")?;
    assert_eq!(
        error,
        fidl_fuchsia_netstack::NetErr {
            status: fidl_fuchsia_netstack::Status::UnknownInterface,
            message: "".to_string(),
        },
    );

    // Don't crash on invalid prefix length.
    interface_address.prefix_len = 43;
    let error = stack
        .add_interface_address(max_id, &mut interface_address)
        .await
        .context("failed to call add interface address")?
        .unwrap_err();
    assert_eq!(error, fidl_fuchsia_net_stack::Error::InvalidArgs);

    let error = netstack
        .remove_interface_address(
            std::convert::TryInto::try_into(max_id).expect("should fit"),
            &mut interface_address.ip_address,
            interface_address.prefix_len,
        )
        .await
        .context("failed to call add interface address")?;
    assert_eq!(
        error,
        fidl_fuchsia_netstack::NetErr {
            status: fidl_fuchsia_netstack::Status::ParseError,
            message: "prefix length exceeds address length".to_string(),
        },
    );

    Ok(())
}

async fn get_interface_info_not_found<N: Netstack>(name: &'static str) -> Result {
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (_env, stack) = sandbox
        .new_netstack::<N, fidl_fuchsia_net_stack::StackMarker>(name)
        .context("failed to create environment")?;

    let interfaces = stack.list_interfaces().await.context("failed to list interfaces")?;
    let max_id = interfaces.iter().map(|interface| interface.id).max().unwrap_or(0);
    let res =
        stack.get_interface_info(max_id + 1).await.context("failed to call get interface info")?;
    assert_eq!(res, Err(fidl_fuchsia_net_stack::Error::NotFound));
    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn get_interface_info_not_found_n2() -> Result {
    get_interface_info_not_found::<Netstack2>("get_interface_info_not_found_n2").await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn get_interface_info_not_found_n3() -> Result {
    get_interface_info_not_found::<Netstack3>("get_interface_info_not_found_n3").await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn disable_interface_loopback() -> Result {
    let name = "disable_interface_loopback";

    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (_env, stack) = sandbox
        .new_netstack::<Netstack2, fidl_fuchsia_net_stack::StackMarker>(name)
        .context("failed to create environment")?;

    let interfaces = stack.list_interfaces().await.context("failed to list interfaces")?;
    let localhost = interfaces
        .iter()
        .find(|interface| {
            interface.properties.features & fidl_fuchsia_hardware_ethernet::INFO_FEATURE_LOOPBACK
                != 0
        })
        .ok_or(anyhow::format_err!("failed to find loopback interface"))?;
    assert_eq!(
        localhost.properties.administrative_status,
        fidl_fuchsia_net_stack::AdministrativeStatus::Enabled
    );
    let () = exec_fidl!(stack.disable_interface(localhost.id), "failed to disable interface")?;
    let info = exec_fidl!(stack.get_interface_info(localhost.id), "failed to get interface info")?;
    assert_eq!(
        info.properties.administrative_status,
        fidl_fuchsia_net_stack::AdministrativeStatus::Disabled
    );
    Ok(())
}

/// Tests fuchsia.net.stack/Stack.del_ethernet_interface.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_remove_interface() -> Result {
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (_env, stack, device) = sandbox
        .new_netstack_and_device::<Netstack2, fidl_fuchsia_net_stack::StackMarker>(
            "test_remove_interface",
        )
        .await
        .context("failed to create netstack environment")?;

    let id = device.add_to_stack(&stack).await.context("failed to add device")?;

    let () = stack
        .del_ethernet_interface(id)
        .await
        .squash_result()
        .context("failed to delete device")?;

    let ifs = stack.list_interfaces().await.context("failed to list interfaces")?;

    assert_eq!(ifs.into_iter().find(|info| info.id == id), None);

    Ok(())
}

/// Tests that adding an interface causes an interface changed event.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_add_interface_causes_interfaces_changed() -> Result {
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (env, stack, device) = sandbox
        .new_netstack_and_device::<Netstack2, fidl_fuchsia_net_stack::StackMarker>(
            "test_remove_interface",
        )
        .await
        .context("failed to create netstack environment")?;

    let netstack = env
        .connect_to_service::<fidl_fuchsia_netstack::NetstackMarker>()
        .context("failed to connect to netstack")?;

    // Ensure we're connected to Netstack so we don't miss any events.
    // Since we do it, assert that only loopback exists.
    let ifs = netstack.get_interfaces2().await.context("failed to get interfaces")?;
    assert_eq!(ifs.len(), 1);

    let id = device.add_to_stack(&stack).await.context("failed to add device")?;

    // Wait for interfaces changed event with the new ID.
    let _ifaces = netstack
        .take_event_stream()
        .try_filter(|fidl_fuchsia_netstack::NetstackEvent::OnInterfacesChanged { interfaces }| {
            futures::future::ready(interfaces.into_iter().any(|iface| iface.id == id as u32))
        })
        .try_next()
        .await
        .context("failed to observe interface addition")?
        .ok_or_else(|| anyhow::anyhow!("netstack stream ended unexpectedly"))?;

    Ok(())
}

/// Tests that if a device closes (is removed from the system), the
/// corresponding Netstack interface is deleted.
/// if `enabled` is `true`, enables the interface before closing the device.
async fn test_close_interface(enabled: bool) -> Result {
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let (env, stack, device) = sandbox
        .new_netstack_and_device::<Netstack2, fidl_fuchsia_net_stack::StackMarker>(
            "test_remove_interface",
        )
        .await
        .context("failed to create netstack environment")?;

    let netstack = env
        .connect_to_service::<fidl_fuchsia_netstack::NetstackMarker>()
        .context("failed to connect to netstack")?;

    // Ensure we're connected to Netstack so we don't miss any events.
    // Since we do it, assert that only loopback exists.
    let ifs = netstack.get_interfaces2().await.context("failed to get interfaces")?;
    assert_eq!(ifs.len(), 1);

    let id = device.add_to_stack(&stack).await.context("failed to add device")?;
    if enabled {
        let () = stack
            .enable_interface(id)
            .await
            .squash_result()
            .context("failed to enable interface")?;
    }

    let _ifaces = netstack
        .take_event_stream()
        .try_filter(|fidl_fuchsia_netstack::NetstackEvent::OnInterfacesChanged { interfaces }| {
            futures::future::ready(interfaces.into_iter().any(|iface| iface.id == id as u32))
        })
        .try_next()
        .await
        .context("failed to observe interface addition")?
        .ok_or_else(|| anyhow::anyhow!("netstack stream ended unexpectedly"))?;

    // Drop the device, that should cause the interface to be deleted.
    std::mem::drop(device);

    // Wait until we observe an event where the removed interface is missing.
    let _ifaces = netstack
        .take_event_stream()
        .try_filter(|fidl_fuchsia_netstack::NetstackEvent::OnInterfacesChanged { interfaces }| {
            println!("interfaces changed: {:?}", interfaces);
            futures::future::ready(!interfaces.into_iter().any(|iface| iface.id == id as u32))
        })
        .try_next()
        .await
        .context("failed to observe interface addition")?
        .ok_or_else(|| anyhow::anyhow!("netstack stream ended unexpectedly"))?;

    Ok(())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_close_disabled_interface() -> Result {
    test_close_interface(false).await
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_close_enabled_interface() -> Result {
    test_close_interface(true).await
}

/// Tests races between device link down and close.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_down_close_race() -> Result {
    let sandbox = TestSandbox::new().context("failed to create sandbox")?;
    let env = sandbox
        .create_netstack_environment::<Netstack2, _>("test_down_close_race")
        .context("failed to create netstack environment")?;

    let netstack = env
        .connect_to_service::<fidl_fuchsia_netstack::NetstackMarker>()
        .context("failed to connect to netstack")?;

    for _ in 0..100u64 {
        let dev = sandbox
            .create_endpoint("ep")
            .await
            .context("failed to create endpoint")?
            .into_interface_in_environment(&env)
            .await
            .context("failed to add endpoint to Netstack")?;

        let () = dev.enable_interface().await.context("failed to enable interface")?;
        let () = dev.start_dhcp().await.context("failed to start DHCP")?;
        let () = dev.set_link_up(true).await.context("failed to bring device up")?;

        let id = dev.id();
        // Wait until the interface is installed and the link state is up.
        let _ifaces = netstack
            .take_event_stream()
            .try_filter(
                |fidl_fuchsia_netstack::NetstackEvent::OnInterfacesChanged { interfaces }| {
                    futures::future::ready(interfaces.into_iter().any(|iface| {
                        iface.id == id as u32
                            && iface.flags & fidl_fuchsia_netstack::NET_INTERFACE_FLAG_UP != 0
                    }))
                },
            )
            .try_next()
            .await
            .context("failed to observe event stream")?
            .ok_or_else(|| {
                anyhow::anyhow!("netstack stream ended unexpectedly while waiting for interface up")
            })?;

        // Here's where we cause the race. We bring the device's link down
        // and drop it right after; the two signals will race to reach
        // Netstack.
        let () = dev.set_link_up(false).await.context("failed to bring link down")?;
        std::mem::drop(dev);

        // Wait until the interface is removed from Netstack cleanly.
        let _ifaces = netstack
            .take_event_stream()
            .try_filter(
                |fidl_fuchsia_netstack::NetstackEvent::OnInterfacesChanged { interfaces }| {
                    futures::future::ready(
                        !interfaces.into_iter().any(|iface| iface.id == id as u32),
                    )
                },
            )
            .try_next()
            .await
            .context("failed to observe event stream")?
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "netstack stream ended unexpectedly while waiting for interface disappear"
                )
            })?;
    }
    Ok(())
}
