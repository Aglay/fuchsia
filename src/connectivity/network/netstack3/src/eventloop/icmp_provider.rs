// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon as zx;
use log::{error, trace};
use rand::RngCore;

use net_types::ip::{IpAddr, IpAddress};
use net_types::SpecifiedAddr;

use fidl_fuchsia_net_icmp::{EchoSocketConfig, EchoSocketRequestStream, ProviderRequest};

use netstack3_core::icmp as core_icmp;
use netstack3_core::{Context, EventDispatcher};

use crate::eventloop::util::CoreCompatible;
use crate::eventloop::{EventLoop, EventLoopInner};

/// Handle a fuchsia.net.icmp.Provider FIDL request, which are used for opening ICMP sockets.
pub fn handle_request(event_loop: &mut EventLoop, req: ProviderRequest) -> Result<(), fidl::Error> {
    match req {
        ProviderRequest::OpenEchoSocket { config, socket, control_handle: _ } => {
            let (stream, handle) = socket.into_stream_and_control_handle()?;
            match open_echo_socket(&mut event_loop.ctx, config, stream) {
                Ok(()) => handle.send_on_open_(zx::Status::OK.into_raw()),
                Err(status) => handle.send_on_open_(status.into_raw()),
            }
        }
    }
}

fn open_echo_socket(
    ctx: &mut Context<EventLoopInner>,
    config: EchoSocketConfig,
    stream: EchoSocketRequestStream,
) -> Result<(), zx::Status> {
    trace!("Opening ICMP Echo socket: {:?}", config);

    let remote: SpecifiedAddr<IpAddr> = config
        .remote
        .ok_or(zx::Status::INVALID_ARGS)?
        .try_into_core()
        .map_err(|_| zx::Status::INVALID_ARGS)?;

    let local: Option<SpecifiedAddr<IpAddr>> = match config.local {
        Some(l) => Some(l.try_into_core().map_err(|_| zx::Status::INVALID_ARGS)?),
        None => None,
    };

    use net_types::ip::IpAddr::{V4, V6};
    match local {
        Some(local) => match (local.into(), remote.into()) {
            (V4(local), V4(remote)) => connect_echo_socket(ctx, stream, Some(local), remote),
            (V6(local), V6(remote)) => connect_echo_socket(ctx, stream, Some(local), remote),
            _ => Err(zx::Status::INVALID_ARGS),
        },
        None => match remote.into() {
            V4(remote) => connect_echo_socket(ctx, stream, None, remote),
            V6(remote) => connect_echo_socket(ctx, stream, None, remote),
        },
    }
}

fn connect_echo_socket<A: IpAddress>(
    ctx: &mut Context<EventLoopInner>,
    stream: EchoSocketRequestStream,
    local: Option<SpecifiedAddr<A>>,
    remote: SpecifiedAddr<A>,
) -> Result<(), zx::Status> {
    // TODO(fxb/36212): Generate icmp_ids without relying on RNG. This line of code does not handle
    // conflicts very well, requiring the client to continuously create sockets until it succeeds.
    let icmp_id = ctx.dispatcher_mut().rng().next_u32() as u16;
    connect_echo_socket_inner(ctx, stream, local, remote, icmp_id)
}

fn connect_echo_socket_inner<A: IpAddress>(
    ctx: &mut Context<EventLoopInner>,
    _stream: EchoSocketRequestStream,
    local: Option<SpecifiedAddr<A>>,
    remote: SpecifiedAddr<A>,
    icmp_id: u16,
) -> Result<(), zx::Status> {
    match core_icmp::new_icmp_connection(ctx, local, remote, icmp_id) {
        Ok(_conn) => {
            // TODO(sbalana): Spawn an EchoSocketWorker to handle requests.
            Ok(())
        }
        Err(e) => {
            error!("Cannot create ICMP connection: {:?}", e);
            Err(zx::Status::ALREADY_EXISTS)
        }
    }
}

#[cfg(test)]
mod test {
    use fuchsia_zircon as zx;
    use futures::stream::StreamExt;
    use log::debug;

    use fidl_fuchsia_net as fidl_net;
    use fidl_fuchsia_net_icmp::{EchoSocketConfig, EchoSocketEvent, EchoSocketMarker};

    use net_types::ip::{AddrSubnetEither, Ipv4Addr};
    use net_types::{SpecifiedAddr, Witness};

    use super::connect_echo_socket_inner;
    use crate::eventloop::integration_tests::{
        new_ipv4_addr_subnet, new_ipv6_addr_subnet, StackSetupBuilder, TestSetupBuilder,
    };

    /// `TestAddr` abstracts extraction of IP addresses (or lack thereof) for testing. This eases
    /// the process of testing different permutations of IP versions.
    trait TestAddr {
        fn local_subnet() -> Option<AddrSubnetEither>;
        fn remote_subnet() -> Option<AddrSubnetEither>;

        fn local_fidl() -> Option<fidl_net::IpAddress>;
        fn remote_fidl() -> Option<fidl_net::IpAddress>;
    }

    struct TestIpv4Addr;
    impl TestAddr for TestIpv4Addr {
        fn local_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv4_addr_subnet([192, 168, 0, 1], 24))
        }
        fn remote_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv4_addr_subnet([192, 168, 0, 2], 24))
        }

        fn local_fidl() -> Option<fidl_net::IpAddress> {
            Some(fidl_net::IpAddress::Ipv4(fidl_net::Ipv4Address { addr: [192, 168, 0, 1] }))
        }
        fn remote_fidl() -> Option<fidl_net::IpAddress> {
            Some(fidl_net::IpAddress::Ipv4(fidl_net::Ipv4Address { addr: [192, 168, 0, 2] }))
        }
    }

    struct TestIpv6Addr;
    impl TestAddr for TestIpv6Addr {
        fn local_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv6_addr_subnet([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2], 64))
        }
        fn remote_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv6_addr_subnet([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3], 64))
        }

        fn local_fidl() -> Option<fidl_net::IpAddress> {
            Some(fidl_net::IpAddress::Ipv6(fidl_net::Ipv6Address {
                addr: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2],
            }))
        }
        fn remote_fidl() -> Option<fidl_net::IpAddress> {
            Some(fidl_net::IpAddress::Ipv6(fidl_net::Ipv6Address {
                addr: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3],
            }))
        }
    }

    struct TestNoIpv4Addr;
    impl TestAddr for TestNoIpv4Addr {
        fn local_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv4_addr_subnet([192, 168, 0, 1], 24))
        }
        fn remote_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv4_addr_subnet([192, 168, 0, 2], 24))
        }

        fn local_fidl() -> Option<fidl_net::IpAddress> {
            None
        }
        fn remote_fidl() -> Option<fidl_net::IpAddress> {
            None
        }
    }

    struct TestNoIpv6Addr;
    impl TestAddr for TestNoIpv6Addr {
        fn local_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv6_addr_subnet([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2], 128))
        }
        fn remote_subnet() -> Option<AddrSubnetEither> {
            Some(new_ipv6_addr_subnet([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3], 128))
        }

        fn local_fidl() -> Option<fidl_net::IpAddress> {
            None
        }
        fn remote_fidl() -> Option<fidl_net::IpAddress> {
            None
        }
    }

    const ALICE: usize = 0;
    const BOB: usize = 1;

    async fn open_icmp_echo_socket<Src: TestAddr, Dst: TestAddr>(expected_status: zx::Status) {
        let mut t = TestSetupBuilder::new()
            .add_named_endpoint("alice")
            .add_named_endpoint("bob")
            .add_stack(StackSetupBuilder::new().add_named_endpoint("alice", Src::local_subnet()))
            .add_stack(StackSetupBuilder::new().add_named_endpoint("bob", Dst::remote_subnet()))
            .build()
            .await
            .expect("Test Setup succeeds");

        // Wait for interfaces on both stacks to signal online correctly
        t.get(ALICE).wait_for_interface_online(1).await;
        t.get(BOB).wait_for_interface_online(1).await;

        let icmp_provider = t.get(ALICE).connect_icmp_provider().unwrap();
        let config = EchoSocketConfig { local: Src::local_fidl(), remote: Dst::remote_fidl() };

        let (socket_client, socket_server) =
            fidl::endpoints::create_endpoints::<EchoSocketMarker>().unwrap();
        let socket = socket_client.into_proxy().unwrap();
        let mut event_stream = socket.take_event_stream();

        icmp_provider.open_echo_socket(config, socket_server).expect("ICMP Echo socket opens");

        // Wait for the ICMP echo socket to open
        loop {
            match t.run_until(event_stream.next()).await.unwrap().unwrap().unwrap() {
                EchoSocketEvent::OnOpen_ { s } => {
                    let status = zx::Status::from_raw(s);
                    debug!("ICMP Echo socket opened with status: {}", status);
                    assert_eq!(status, expected_status);
                    break;
                }
            }
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv4() {
        open_icmp_echo_socket::<TestIpv4Addr, TestIpv4Addr>(zx::Status::OK).await;
        // TODO(sbalana): Send ICMP echoes from Source to Destination
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv6() {
        open_icmp_echo_socket::<TestIpv6Addr, TestIpv6Addr>(zx::Status::OK).await;
        // TODO(sbalana): Send ICMP echoes from Source to Destination
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv4_no_local_ip() {
        open_icmp_echo_socket::<TestNoIpv4Addr, TestIpv4Addr>(zx::Status::OK).await;
        // TODO(sbalana): Send ICMP echoes from Source to Destination
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv6_no_local_ip() {
        open_icmp_echo_socket::<TestNoIpv6Addr, TestIpv6Addr>(zx::Status::OK).await;
        // TODO(sbalana): Send ICMP echoes from Source to Destination
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv4_no_remote_ip() {
        open_icmp_echo_socket::<TestIpv4Addr, TestNoIpv4Addr>(zx::Status::INVALID_ARGS).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv6_no_remote_ip() {
        open_icmp_echo_socket::<TestIpv6Addr, TestNoIpv6Addr>(zx::Status::INVALID_ARGS).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv4_ipv6_mismatch() {
        open_icmp_echo_socket::<TestIpv4Addr, TestIpv6Addr>(zx::Status::INVALID_ARGS).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_ipv6_ipv4_mismatch() {
        open_icmp_echo_socket::<TestIpv6Addr, TestIpv4Addr>(zx::Status::INVALID_ARGS).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_no_local_or_remote_ipv4() {
        open_icmp_echo_socket::<TestNoIpv4Addr, TestNoIpv4Addr>(zx::Status::INVALID_ARGS).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_no_local_or_remote_ipv6() {
        open_icmp_echo_socket::<TestNoIpv6Addr, TestNoIpv6Addr>(zx::Status::INVALID_ARGS).await;
    }

    // Relies on connect_echo_socket_inner, thus cannot use `open_icmp_echo_socket`.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_icmp_echo_socket_duplicate() {
        const ALICE: usize = 0;
        const BOB: usize = 1;
        const ALICE_IP: [u8; 4] = [192, 168, 0, 1];
        const BOB_IP: [u8; 4] = [192, 168, 0, 2];

        let mut t = TestSetupBuilder::new()
            .add_named_endpoint("alice")
            .add_named_endpoint("bob")
            .add_stack(
                StackSetupBuilder::new()
                    .add_named_endpoint("alice", Some(new_ipv4_addr_subnet(ALICE_IP, 24))),
            )
            .add_stack(
                StackSetupBuilder::new()
                    .add_named_endpoint("bob", Some(new_ipv4_addr_subnet(BOB_IP, 24))),
            )
            .build()
            .await
            .expect("Test Setup succeeds");

        // Wait for interfaces on both stacks to signal online correctly
        t.get(ALICE).wait_for_interface_online(1).await;
        t.get(BOB).wait_for_interface_online(1).await;

        // Open an ICMP echo socket from Alice to Bob
        t.get(ALICE).connect_icmp_provider().unwrap();

        let local = Some(SpecifiedAddr::new(Ipv4Addr::new(ALICE_IP)).unwrap());
        let remote = SpecifiedAddr::new(Ipv4Addr::new(BOB_IP)).unwrap();

        let (_, socket_server) = fidl::endpoints::create_endpoints::<EchoSocketMarker>().unwrap();
        let request_stream = socket_server.into_stream().unwrap();

        assert_eq!(
            connect_echo_socket_inner(
                &mut t.event_loop(ALICE).ctx,
                request_stream,
                local,
                remote,
                1,
            ),
            Ok(())
        );

        // Open another ICMP echo socket from Alice to Bob with same connection identifier
        let (_, socket_server) = fidl::endpoints::create_endpoints::<EchoSocketMarker>().unwrap();
        let request_stream = socket_server.into_stream().unwrap();

        assert_eq!(
            connect_echo_socket_inner(
                &mut t.event_loop(ALICE).ctx,
                request_stream,
                local,
                remote,
                1,
            ),
            Err(zx::Status::ALREADY_EXISTS)
        );
    }
}
