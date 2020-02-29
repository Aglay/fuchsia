// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! IPv4 and IPv6 sockets.

use core::marker::PhantomData;

use net_types::ip::{Ip, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr};
use net_types::SpecifiedAddr;
use packet::{BufferMut, Serializer};

use crate::device::DeviceId;
use crate::error::NoRouteError;
use crate::ip::forwarding::ForwardingTable;
use crate::ip::{IpExt, IpProto};
use crate::wire::{ipv4::Ipv4PacketBuilder, ipv6::Ipv6PacketBuilder};
use crate::{BufferDispatcher, Context, EventDispatcher};

/// A socket identifying a connection between a local and remote IP host.
///
/// `IpSocket`s may cache certain routing information that is used to speed up
/// the operation of sending outbound packets. However, this means that updates
/// to global state (for example, updates to the forwarding table) may
/// invalidate that cached information. Thus, certain updates may require
/// updating all stored sockets as well. See the `Update` and `UpdateMeta`
/// associated types and the `apply_update` method for more details.
pub(crate) trait IpSocket<I: Ip> {
    /// The type of updates to the socket.
    ///
    /// Updates are emitted whenever information changes that might require
    /// information cached in sockets to be updated. For example, changes to the
    /// forwarding table might require that a socket's outbound device be
    /// updated.
    type Update;

    /// Metadata required to perform an update.
    ///
    /// Extra metadata may be required in order to apply an update to a socket.
    type UpdateMeta;

    /// Get the local IP address.
    fn local_ip(&self) -> &SpecifiedAddr<I::Addr>;

    /// Get the remote IP address.
    fn remote_ip(&self) -> &SpecifiedAddr<I::Addr>;

    /// Apply an update to the socket.
    ///
    /// `apply_update` applies the given update, possibly changing cached
    /// information. If it returns `Err`, then a) the socket is no longer
    /// routable AND, b) the socket's unroutable behavior is
    /// [`UnroutableBehavior::Close`]. In this case, the socket must be closed.
    /// This is a MUST, not a SHOULD, as the cached information is now invalid,
    /// and the behavior of any further use of the socket is not well-defined.
    /// It is the caller's responsibility to ensure that the socket is no longer
    /// used, including instructing the bindings not to use it again.
    ///
    /// If the socket is no longer routable but the socket's unroutable behavior
    /// is [`UnroutableBehavior::StayOpen`], then the socket remains open, and
    /// `apply_update` returns `Ok`. So long as the socket is unroutable, calls
    /// to [`BufferIpSocketContext::send_ip_packet`] will fail.
    fn apply_update(
        &mut self,
        update: &Self::Update,
        meta: &Self::UpdateMeta,
    ) -> Result<(), NoRouteError>;
}

/// An execution context defining a type of IP socket.
pub(crate) trait IpSocketContext<I: Ip> {
    // TODO(joshlf): Remove `Clone` bound once we're no longer cloning sockets.
    type IpSocket: IpSocket<I> + Clone;

    /// Constructs a new [`Self::IpSocket`].
    ///
    /// `new_ip_socket` constructs a new `Self::IpSocket` to the given remote IP
    /// address from the given local IP address with the given IP protocol. If
    /// no local IP address is given, one will be chosen automatically.
    ///
    /// `new_ip_socket` returns an error if no route to the remote was found in
    /// the forwarding table, if the given local IP address is not valid for the
    /// found route, or if the remote is a loopback address (which is not
    /// currently supported, but will be in the future). Currently, this is true
    /// regardless of the value of `unroutable_behavior`. Eventually, this will
    /// be changed.
    ///
    /// Outbound frames will have the given `ttl` or a default value if `ttl` is
    /// `None`.
    fn new_ip_socket(
        &mut self,
        local_ip: Option<SpecifiedAddr<I::Addr>>,
        remote_ip: SpecifiedAddr<I::Addr>,
        proto: IpProto,
        ttl: Option<u8>,
        unroutable_behavior: UnroutableBehavior,
    ) -> Result<Self::IpSocket, NoRouteError>;
}

// TODO(joshlf): Use FrameContext instead?

/// An error in sending a packet on an IP socket.
#[derive(Debug, Eq, PartialEq)]
pub enum SendError {
    /// An MTU was exceeded.
    ///
    /// This could be caused by an MTU at any layer of the stack, including both
    /// device MTUs and packet format body size limits.
    Mtu,
    /// The socket is currently unroutable.
    Unroutable,
}

pub(crate) trait BufferIpSocketContext<I: Ip, B: BufferMut>: IpSocketContext<I> {
    /// Send an IP packet on a socket.
    ///
    /// The generated packet has its metadata initialized from `socket`,
    /// including the source and destination addresses, the Time To Live/Hop
    /// Limit, and the Protocol/Next Header. The outbound device is also chosen
    /// based on information stored in the socket.
    ///
    /// If the socket is currently unroutable, an error is returned.
    fn send_ip_packet<S: Serializer<Buffer = B>>(
        &mut self,
        socket: &Self::IpSocket,
        body: S,
    ) -> Result<(), (S, SendError)>;
}

/// What should a socket do when it becomes unroutable?
///
/// `UnroutableBehavior` describes how a socket is configured to behave if it
/// becomes unroutable.
#[derive(Copy, Clone, Eq, PartialEq)]
#[allow(unused)]
pub(crate) enum UnroutableBehavior {
    /// The socket should stay open. Attempting to send a packet while the
    /// socket is unroutable will return an error on a per-packet basis. If the
    /// socket later becomes routable again, these operations will succeed
    /// again.
    StayOpen,
    /// The socket should close.
    Close,
}

/// The production implementation of the [`IpSocket`] trait.
#[derive(Clone)]
#[cfg_attr(test, derive(Eq, PartialEq))]
pub(crate) struct IpSock<I: IpExt, D> {
    // TODO(joshlf): This struct is larger than it needs to be. `remote_ip`,
    // `local_ip`, and `proto` are all stored in `cached`'s `builder` when it's
    // `Routable`. Instead, we could move these into the `Unroutable` variant of
    // `CachedInfo`.

    //
    // These are part of the socket definition and never change.
    //
    remote_ip: SpecifiedAddr<I::Addr>,
    // Guaranteed to be unicast in its subnet since it's always equal to an
    // address assigned to the local device. We can't use the `UnicastAddr`
    // witness type since `Ipv4Addr` doesn't implement `UnicastAddress`.
    //
    // TODO(joshlf): Support unnumbered interfaces. Once we do that, a few
    // issues arise: A) Does the unicast restriction still apply, and is that
    // even well-defined for IPv4 in the absence of a subnet? B) Presumably we
    // have to always bind to a particular interface?
    local_ip: SpecifiedAddr<I::Addr>,
    proto: IpProto,
    unroutable_behavior: UnroutableBehavior,

    //
    // This is merely cached and can change.
    //

    // This is `Routable` if the socket is currently routable and `Unroutable`
    // otherwise. If `unroutable_behavior` is `Close`, then this is guaranteed
    // to be `Routable`.
    cached: CachedInfo<I, D>,
}

/// Information which is cached inside an [`IpSock`].
#[derive(Clone)]
#[cfg_attr(test, derive(Eq, PartialEq))]
#[allow(unused)]
enum CachedInfo<I: IpExt, D> {
    Routable { builder: I::PacketBuilder, device: D, next_hop: SpecifiedAddr<I::Addr> },
    Unroutable,
}

/// An update to the information cached in an [`IpSock`].
///
/// Whenever IP-layer information changes that might affect `IpSock`s, an
/// `IpSockUpdate` is emitted, and clients are responsible for applying this
/// update to all `IpSock`s that they are responsible for.
pub(crate) struct IpSockUpdate<I: IpExt> {
    // Currently, `IpSockUpdate`s only represent a single type of update: that
    // the forwarding table or assignment of IP addresses to devices have
    // changed in some way. Thus, this is a ZST.
    _marker: PhantomData<I>,
}

impl<I: IpExt> IpSockUpdate<I> {
    /// Constructs a new `IpSocketUpdate`.
    pub(crate) fn new() -> IpSockUpdate<I> {
        IpSockUpdate { _marker: PhantomData }
    }
}

impl<I: IpExt, D> IpSocket<I> for IpSock<I, D> {
    type Update = IpSockUpdate<I>;
    type UpdateMeta = ForwardingTable<I, D>;

    fn local_ip(&self) -> &SpecifiedAddr<I::Addr> {
        &self.local_ip
    }

    fn remote_ip(&self) -> &SpecifiedAddr<I::Addr> {
        &self.remote_ip
    }

    fn apply_update(
        &mut self,
        _update: &IpSockUpdate<I>,
        _meta: &ForwardingTable<I, D>,
    ) -> Result<(), NoRouteError> {
        // NOTE(joshlf): Currently, with this unimplemented, we will panic if we
        // ever try to update the forwarding table or the assignment of IP
        // addresses to devices while sockets are installed. However, updates
        // that happen while no sockets are installed will still succeed. This
        // should allow us to continue testing Netstack3 until we implement this
        // method.
        unimplemented!()
    }
}

/// Apply an update to all IPv4 sockets.
///
/// `apply_ipv4_socket_update` applies the given socket update to all IPv4
/// sockets in existence. It does this by delegating to every module that is
/// responsible for storing IPv4 sockets.
pub(crate) fn apply_ipv4_socket_update<D: EventDispatcher>(
    ctx: &mut Context<D>,
    update: IpSockUpdate<Ipv4>,
) {
    crate::ip::icmp::apply_ipv4_socket_update(ctx, update);
}

/// Apply an update to all IPv6 sockets.
///
/// `apply_ipv6_socket_update` applies the given socket update to all IPv6
/// sockets in existence. It does this by delegating to every module that is
/// responsible for storing IPv6 sockets.
pub(crate) fn apply_ipv6_socket_update<D: EventDispatcher>(
    ctx: &mut Context<D>,
    update: IpSockUpdate<Ipv6>,
) {
    crate::ip::icmp::apply_ipv6_socket_update(ctx, update);
}

// TODO(joshlf): Once we support configuring transport-layer protocols using
// type parameters, use that to ensure that `proto` is the right protocol for
// the caller. We will still need to have a separate enforcement mechanism for
// raw IP sockets once we support those.

impl<D: EventDispatcher> IpSocketContext<Ipv4> for Context<D> {
    type IpSocket = IpSock<Ipv4, DeviceId>;

    fn new_ip_socket(
        &mut self,
        local_ip: Option<SpecifiedAddr<Ipv4Addr>>,
        remote_ip: SpecifiedAddr<Ipv4Addr>,
        proto: IpProto,
        ttl: Option<u8>,
        unroutable_behavior: UnroutableBehavior,
    ) -> Result<IpSock<Ipv4, DeviceId>, NoRouteError> {
        if Ipv4::LOOPBACK_SUBNET.contains(&remote_ip) {
            return Err(NoRouteError);
        }

        super::lookup_route(self, remote_ip).ok_or(NoRouteError).and_then(|dst| {
            let local_ip = if let Some(local_ip) = local_ip {
                if crate::device::get_ip_addr_subnets::<_, Ipv4Addr>(self, dst.device)
                    .any(|addr_sub| local_ip == addr_sub.addr())
                {
                    local_ip
                } else {
                    return Err(NoRouteError);
                }
            } else {
                // TODO(joshlf): Are we sure that a device route can never be
                // set for a device without an IP address? At the least, this is
                // not currently enforced anywhere, and is a DoS vector.
                crate::device::get_ip_addr_subnet(self, dst.device)
                    .expect("IP device route set for device without IP address")
                    .addr()
            };
            Ok(IpSock {
                // `get_ip_addr_subnet` and `get_ip_addr_subnets` both return
                // `AddrSubnet`s, which guarantee that their addresses are
                // unicast address in their subnets. That satisfies the
                // invariant on this field.
                local_ip,
                remote_ip,
                proto,
                unroutable_behavior,
                cached: CachedInfo::Routable {
                    builder: Ipv4PacketBuilder::new(
                        local_ip,
                        remote_ip,
                        ttl.unwrap_or(super::DEFAULT_TTL),
                        proto,
                    ),
                    device: dst.device,
                    next_hop: dst.next_hop,
                },
            })
        })
    }
}

impl<D: EventDispatcher> IpSocketContext<Ipv6> for Context<D> {
    type IpSocket = IpSock<Ipv6, DeviceId>;

    fn new_ip_socket(
        &mut self,
        local_ip: Option<SpecifiedAddr<Ipv6Addr>>,
        remote_ip: SpecifiedAddr<Ipv6Addr>,
        proto: IpProto,
        hop_limit: Option<u8>,
        unroutable_behavior: UnroutableBehavior,
    ) -> Result<IpSock<Ipv6, DeviceId>, NoRouteError> {
        if Ipv6::LOOPBACK_SUBNET.contains(&remote_ip) {
            return Err(NoRouteError);
        }

        super::lookup_route(self, remote_ip).ok_or(NoRouteError).and_then(|dst| {
            let local_ip = if let Some(local_ip) = local_ip {
                if crate::device::get_ip_addr_state(self, dst.device, &local_ip).is_some() {
                    local_ip
                } else {
                    return Err(NoRouteError);
                }
            } else {
                // TODO(joshlf): Are we sure that a device route can never be
                // set for a device without an IP address? At the least, this is
                // not currently enforced anywhere, and is a DoS vector.
                crate::device::get_ip_addr_subnet(self, dst.device)
                    .expect("IP device route set for device without IP address")
                    .addr()
            };
            Ok(IpSock {
                // `get_ip_addr_subnet` and `get_ip_addr_subnets` both return
                // `AddrSubnet`s, which guarantee that their addresses are
                // unicast address in their subnets. That satisfies the
                // invariant on this field.
                local_ip,
                remote_ip,
                proto,
                unroutable_behavior,
                cached: CachedInfo::Routable {
                    builder: Ipv6PacketBuilder::new(
                        local_ip,
                        remote_ip,
                        hop_limit.unwrap_or(super::DEFAULT_TTL),
                        proto,
                    ),
                    device: dst.device,
                    next_hop: dst.next_hop,
                },
            })
        })
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>> BufferIpSocketContext<Ipv4, B> for Context<D> {
    fn send_ip_packet<S: Serializer<Buffer = B>>(
        &mut self,
        socket: &IpSock<Ipv4, DeviceId>,
        body: S,
    ) -> Result<(), (S, SendError)> {
        // TODO(joshlf): Call `trace!` with relevant fields from the socket.
        increment_counter!(self, "send_ipv4_packet");
        // TODO(joshlf): Handle loopback sockets.
        assert!(!Ipv4::LOOPBACK_SUBNET.contains(&socket.remote_ip));
        // If the remote IP is non-loopback but the local IP is loopback, that
        // implies a bug elsewhere - when we resolve the local IP in
        // `new_ipv4_socket`, if the remote IP is not a loopback IP, then we
        // should never choose a loopback IP as our local IP.
        assert!(!Ipv4::LOOPBACK_SUBNET.contains(&socket.local_ip));

        match &socket.cached {
            CachedInfo::Routable { builder, device, next_hop } => {
                // Tentative addresses are not considered bound to an interface
                // in the traditional sense. Therefore, no packet should have a
                // source IP set to a tentative address. This should be enforced
                // by the `IpSock` being kept up to date.
                debug_assert!(!crate::device::is_addr_tentative_on_device(
                    self,
                    &socket.local_ip,
                    *device
                ));

                crate::device::send_ip_frame(self, *device, *next_hop, body.encapsulate(builder))
                    .map_err(|ser| (ser.into_inner(), SendError::Mtu))
            }
            CachedInfo::Unroutable => Err((body, SendError::Unroutable)),
        }
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>> BufferIpSocketContext<Ipv6, B> for Context<D> {
    fn send_ip_packet<S: Serializer<Buffer = B>>(
        &mut self,
        socket: &IpSock<Ipv6, DeviceId>,
        body: S,
    ) -> Result<(), (S, SendError)> {
        // TODO(joshlf): Call `trace!` with relevant fields from the socket.
        increment_counter!(self, "send_ipv6_packet");
        // TODO(joshlf): Handle loopback sockets.
        assert!(!Ipv6::LOOPBACK_SUBNET.contains(&socket.remote_ip));
        // If the remote IP is non-loopback but the local IP is loopback, that
        // implies a bug elsewhere - when we resolve the local IP in
        // `new_ipv6_socket`, if the remote IP is not a loopback IP, then we
        // should never choose a loopback IP as our local IP.
        assert!(!Ipv6::LOOPBACK_SUBNET.contains(&socket.local_ip));

        match &socket.cached {
            CachedInfo::Routable { builder, device, next_hop } => {
                // Tentative addresses are not considered bound to an interface
                // in the traditional sense. Therefore, no packet should have a
                // source IP set to a tentative address. This should be enforced
                // by the `IpSock` being kept up to date.
                debug_assert!(!crate::device::is_addr_tentative_on_device(
                    self,
                    &socket.local_ip,
                    *device
                ));

                crate::device::send_ip_frame(self, *device, *next_hop, body.encapsulate(builder))
                    .map_err(|ser| (ser.into_inner(), SendError::Mtu))
            }
            CachedInfo::Unroutable => Err((body, SendError::Unroutable)),
        }
    }
}

/// Test mock implementations of the traits defined in the `socket` module.
#[cfg(test)]
pub(crate) mod testutil {
    use net_types::ip::IpAddress;

    use super::*;
    use crate::context::testutil::DummyContext;

    /// A dummy implementation of [`IpSocket`].
    #[derive(Clone)]
    pub(crate) struct DummyIpSocket<A: IpAddress> {
        local_ip: SpecifiedAddr<A>,
        remote_ip: SpecifiedAddr<A>,
        proto: IpProto,
        ttl: u8,
        unroutable_behavior: UnroutableBehavior,
        // Guaranteed to be `true` if `unroutable_behavior` is `Close`.
        routable: bool,
    }

    /// An update to a [`DummyIpSocket`].
    pub(crate) struct DummyIpSocketUpdate<I: Ip> {
        // The value to set the socket's `routable` field to.
        routable: bool,
        _marker: PhantomData<I>,
    }

    impl<I: Ip> IpSocket<I> for DummyIpSocket<I::Addr> {
        type Update = DummyIpSocketUpdate<I>;
        type UpdateMeta = ();

        fn local_ip(&self) -> &SpecifiedAddr<I::Addr> {
            &self.local_ip
        }

        fn remote_ip(&self) -> &SpecifiedAddr<I::Addr> {
            &self.remote_ip
        }

        fn apply_update(
            &mut self,
            update: &DummyIpSocketUpdate<I>,
            _meta: &(),
        ) -> Result<(), NoRouteError> {
            if !update.routable && self.unroutable_behavior == UnroutableBehavior::Close {
                return Err(NoRouteError);
            }
            self.routable = update.routable;
            Ok(())
        }
    }

    /// A dummy implementation of [`IpSocketContext`].
    ///
    /// `IpSocketContext` is implemented for any `DummyContext<S>` where `S`
    /// implements `AsRef` and `AsMut` for `DummyIpSocketContext`.
    pub(crate) struct DummyIpSocketContext<I: Ip> {
        // The default value to use if `new_ip_socket` is called with a
        // `local_ip` of `None`.
        default_local_ip: SpecifiedAddr<I::Addr>,
        // Whether or not calls to `new_ip_socket` should succeed.
        routable_at_creation: bool,
    }

    impl<I: Ip> DummyIpSocketContext<I> {
        /// Creates a new `DummyIpSocketContext`.
        ///
        /// `default_local_ip` is the local IP address to use if none is
        /// specified in a request to create a new IP socket.
        /// `routable_at_creation` controls whether or not calls to
        /// `new_ip_socket` should succeed.
        pub(crate) fn new(
            default_local_ip: SpecifiedAddr<I::Addr>,
            routable_at_creation: bool,
        ) -> DummyIpSocketContext<I> {
            DummyIpSocketContext { default_local_ip, routable_at_creation }
        }
    }

    impl<I: Ip, S: AsRef<DummyIpSocketContext<I>> + AsMut<DummyIpSocketContext<I>>, Id, Meta>
        IpSocketContext<I> for DummyContext<S, Id, Meta>
    {
        type IpSocket = DummyIpSocket<I::Addr>;

        fn new_ip_socket(
            &mut self,
            local_ip: Option<SpecifiedAddr<I::Addr>>,
            remote_ip: SpecifiedAddr<I::Addr>,
            proto: IpProto,
            ttl: Option<u8>,
            unroutable_behavior: UnroutableBehavior,
        ) -> Result<DummyIpSocket<I::Addr>, NoRouteError> {
            let ctx = self.get_ref().as_ref();

            if !ctx.routable_at_creation {
                return Err(NoRouteError);
            }

            Ok(DummyIpSocket {
                local_ip: local_ip.unwrap_or(ctx.default_local_ip),
                remote_ip,
                proto,
                ttl: ttl.unwrap_or(crate::ip::DEFAULT_TTL),
                unroutable_behavior,
                routable: true,
            })
        }
    }

    impl<
            I: Ip,
            B: BufferMut,
            S: AsRef<DummyIpSocketContext<I>> + AsMut<DummyIpSocketContext<I>>,
            Id,
            Meta,
        > BufferIpSocketContext<I, B> for DummyContext<S, Id, Meta>
    {
        fn send_ip_packet<SS: Serializer<Buffer = B>>(
            &mut self,
            _socket: &Self::IpSocket,
            _body: SS,
        ) -> Result<(), (SS, SendError)> {
            unimplemented!()
        }
    }
}

#[cfg(test)]
mod tests {
    use net_types::Witness;
    use packet::InnerPacketBuilder;

    use super::*;
    use crate::testutil::*;

    #[test]
    fn test_new() {
        // Test that `new_ip_socket` works with various edge cases.

        //
        // IPv4
        //

        let mut ctx = DummyEventDispatcherBuilder::from_config(DUMMY_CONFIG_V4)
            .build::<DummyEventDispatcher>();

        // A template socket that we can use to more concisely define sockets in
        // various test cases.
        let template = IpSock {
            remote_ip: DUMMY_CONFIG_V4.remote_ip,
            local_ip: DUMMY_CONFIG_V4.local_ip,
            proto: IpProto::Icmp,
            unroutable_behavior: UnroutableBehavior::Close,
            cached: CachedInfo::Routable {
                builder: Ipv4PacketBuilder::new(
                    DUMMY_CONFIG_V4.local_ip,
                    DUMMY_CONFIG_V4.remote_ip,
                    crate::ip::DEFAULT_TTL,
                    IpProto::Icmp,
                ),
                device: DeviceId::new_ethernet(0),
                next_hop: DUMMY_CONFIG_V4.remote_ip,
            },
        };

        // All optional fields are `None`.
        assert!(
            IpSocketContext::<Ipv4>::new_ip_socket(
                &mut ctx,
                None,
                DUMMY_CONFIG_V4.remote_ip,
                IpProto::Icmp,
                None,
                UnroutableBehavior::Close
            )
            .unwrap()
                == template
        );

        // TTL is specified.
        assert!(
            IpSocketContext::<Ipv4>::new_ip_socket(
                &mut ctx,
                None,
                DUMMY_CONFIG_V4.remote_ip,
                IpProto::Icmp,
                Some(1),
                UnroutableBehavior::Close
            )
            .unwrap()
                == {
                    // The template socket, but with the TTL set to 1.
                    let mut x = template.clone();
                    x.cached = CachedInfo::Routable {
                        builder: Ipv4PacketBuilder::new(
                            DUMMY_CONFIG_V4.local_ip,
                            DUMMY_CONFIG_V4.remote_ip,
                            1,
                            IpProto::Icmp,
                        ),
                        device: DeviceId::new_ethernet(0),
                        next_hop: DUMMY_CONFIG_V4.remote_ip,
                    };
                    x
                }
        );

        // Local address is specified, and is a valid local address.
        assert!(
            IpSocketContext::<Ipv4>::new_ip_socket(
                &mut ctx,
                Some(DUMMY_CONFIG_V4.local_ip),
                DUMMY_CONFIG_V4.remote_ip,
                IpProto::Icmp,
                None,
                UnroutableBehavior::Close
            )
            .unwrap()
                == template
        );

        // Local address is specified, and is an invalid local address.
        assert!(
            IpSocketContext::<Ipv4>::new_ip_socket(
                &mut ctx,
                Some(DUMMY_CONFIG_V4.remote_ip),
                DUMMY_CONFIG_V4.remote_ip,
                IpProto::Icmp,
                None,
                UnroutableBehavior::Close
            ) == Err(NoRouteError)
        );

        // Loopback sockets are not yet supported.
        assert!(
            IpSocketContext::<Ipv4>::new_ip_socket(
                &mut ctx,
                None,
                Ipv4::LOOPBACK_ADDRESS,
                IpProto::Icmp,
                None,
                UnroutableBehavior::Close
            ) == Err(NoRouteError)
        );

        //
        // IPv6
        //

        let mut ctx = DummyEventDispatcherBuilder::from_config(DUMMY_CONFIG_V6)
            .build::<DummyEventDispatcher>();

        // A template socket that we can use to more concisely define sockets in
        // various test cases.
        let template = IpSock {
            remote_ip: DUMMY_CONFIG_V6.remote_ip,
            local_ip: DUMMY_CONFIG_V6.local_ip,
            proto: IpProto::Icmpv6,
            unroutable_behavior: UnroutableBehavior::Close,
            cached: CachedInfo::Routable {
                builder: Ipv6PacketBuilder::new(
                    DUMMY_CONFIG_V6.local_ip,
                    DUMMY_CONFIG_V6.remote_ip,
                    crate::ip::DEFAULT_TTL,
                    IpProto::Icmpv6,
                ),
                device: DeviceId::new_ethernet(0),
                next_hop: DUMMY_CONFIG_V6.remote_ip,
            },
        };

        // All optional fields are `None`.
        assert!(
            IpSocketContext::<Ipv6>::new_ip_socket(
                &mut ctx,
                None,
                DUMMY_CONFIG_V6.remote_ip,
                IpProto::Icmpv6,
                None,
                UnroutableBehavior::Close
            )
            .unwrap()
                == template
        );

        // TTL is specified.
        assert!(
            IpSocketContext::<Ipv6>::new_ip_socket(
                &mut ctx,
                None,
                DUMMY_CONFIG_V6.remote_ip,
                IpProto::Icmpv6,
                Some(1),
                UnroutableBehavior::Close
            )
            .unwrap()
                == {
                    // The template socket, but with the TTL set to 1.
                    let mut x = template.clone();
                    x.cached = CachedInfo::Routable {
                        builder: Ipv6PacketBuilder::new(
                            DUMMY_CONFIG_V6.local_ip,
                            DUMMY_CONFIG_V6.remote_ip,
                            1,
                            IpProto::Icmpv6,
                        ),
                        device: DeviceId::new_ethernet(0),
                        next_hop: DUMMY_CONFIG_V6.remote_ip,
                    };
                    x
                }
        );

        // Local address is specified, and is a valid local address.
        assert!(
            IpSocketContext::<Ipv6>::new_ip_socket(
                &mut ctx,
                Some(DUMMY_CONFIG_V6.local_ip),
                DUMMY_CONFIG_V6.remote_ip,
                IpProto::Icmpv6,
                None,
                UnroutableBehavior::Close
            )
            .unwrap()
                == template
        );

        // Local address is specified, and is an invalid local address.
        assert!(
            IpSocketContext::<Ipv6>::new_ip_socket(
                &mut ctx,
                Some(DUMMY_CONFIG_V6.remote_ip),
                DUMMY_CONFIG_V6.remote_ip,
                IpProto::Icmpv6,
                None,
                UnroutableBehavior::Close
            ) == Err(NoRouteError)
        );

        // Loopback sockets are not yet supported.
        assert!(
            IpSocketContext::<Ipv6>::new_ip_socket(
                &mut ctx,
                None,
                Ipv6::LOOPBACK_ADDRESS,
                IpProto::Icmpv6,
                None,
                UnroutableBehavior::Close
            ) == Err(NoRouteError)
        );
    }

    #[test]
    fn test_send() {
        // Test various edge cases of the
        // `BufferIpSocketContext::send_ip_packet` method.

        //
        // IPv4
        //

        let mut ctx = DummyEventDispatcherBuilder::from_config(DUMMY_CONFIG_V4)
            .build::<DummyEventDispatcher>();

        // Create a normal, routable socket.
        let mut sock = IpSocketContext::<Ipv4>::new_ip_socket(
            &mut ctx,
            None,
            DUMMY_CONFIG_V4.remote_ip,
            IpProto::Icmp,
            Some(1),
            UnroutableBehavior::Close,
        )
        .unwrap();

        // Send a packet on the socket and make sure that the right contents
        // are sent.
        BufferIpSocketContext::<Ipv4, _>::send_ip_packet(
            &mut ctx,
            &sock,
            (&[0u8][..]).into_serializer(),
        )
        .unwrap();

        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);

        let (dev, frame) = &ctx.dispatcher().frames_sent()[0];
        assert_eq!(dev, &DeviceId::new_ethernet(0));
        let (body, src_mac, dst_mac, src_ip, dst_ip, proto, ttl) =
            parse_ip_packet_in_ethernet_frame::<Ipv4>(&frame).unwrap();
        assert_eq!(body, [0]);
        assert_eq!(src_mac, DUMMY_CONFIG_V4.local_mac);
        assert_eq!(dst_mac, DUMMY_CONFIG_V4.remote_mac);
        assert_eq!(src_ip, DUMMY_CONFIG_V4.local_ip.get());
        assert_eq!(dst_ip, DUMMY_CONFIG_V4.remote_ip.get());
        assert_eq!(proto, IpProto::Icmp);
        assert_eq!(ttl, 1);

        // Try sending a packet which will be larger than the device's MTU,
        // and make sure it fails.
        let body = vec![0u8; crate::ip::IPV6_MIN_MTU as usize];
        assert_eq!(
            BufferIpSocketContext::<Ipv4, _>::send_ip_packet(
                &mut ctx,
                &sock,
                (&body[..]).into_serializer(),
            )
            .unwrap_err()
            .1,
            SendError::Mtu
        );

        // Make sure that sending on an unroutable socket fails.
        sock.cached = CachedInfo::Unroutable;
        assert_eq!(
            BufferIpSocketContext::<Ipv4, _>::send_ip_packet(
                &mut ctx,
                &sock,
                (&[0u8][..]).into_serializer(),
            )
            .unwrap_err()
            .1,
            SendError::Unroutable
        );

        //
        // IPv6
        //

        let mut ctx = DummyEventDispatcherBuilder::from_config(DUMMY_CONFIG_V6)
            .build::<DummyEventDispatcher>();

        // Create a normal, routable socket.
        let mut sock = IpSocketContext::<Ipv6>::new_ip_socket(
            &mut ctx,
            None,
            DUMMY_CONFIG_V6.remote_ip,
            IpProto::Icmpv6,
            Some(1),
            UnroutableBehavior::Close,
        )
        .unwrap();

        // Send a packet on the socket and make sure that the right contents
        // are sent.
        BufferIpSocketContext::<Ipv6, _>::send_ip_packet(
            &mut ctx,
            &sock,
            (&[0u8][..]).into_serializer(),
        )
        .unwrap();

        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);

        let (dev, frame) = &ctx.dispatcher().frames_sent()[0];
        assert_eq!(dev, &DeviceId::new_ethernet(0));
        let (body, src_mac, dst_mac, src_ip, dst_ip, proto, ttl) =
            parse_ip_packet_in_ethernet_frame::<Ipv6>(&frame).unwrap();
        assert_eq!(body, [0]);
        assert_eq!(src_mac, DUMMY_CONFIG_V6.local_mac);
        assert_eq!(dst_mac, DUMMY_CONFIG_V6.remote_mac);
        assert_eq!(src_ip, DUMMY_CONFIG_V6.local_ip.get());
        assert_eq!(dst_ip, DUMMY_CONFIG_V6.remote_ip.get());
        assert_eq!(proto, IpProto::Icmpv6);
        assert_eq!(ttl, 1);

        // Try sending a packet which will be larger than the device's MTU,
        // and make sure it fails.
        let body = vec![0u8; crate::ip::IPV6_MIN_MTU as usize];
        assert_eq!(
            BufferIpSocketContext::<Ipv6, _>::send_ip_packet(
                &mut ctx,
                &sock,
                (&body[..]).into_serializer(),
            )
            .unwrap_err()
            .1,
            SendError::Mtu
        );

        // Make sure that sending on an unroutable socket fails.
        sock.cached = CachedInfo::Unroutable;
        assert_eq!(
            BufferIpSocketContext::<Ipv6, _>::send_ip_packet(
                &mut ctx,
                &sock,
                (&[0u8][..]).into_serializer(),
            )
            .unwrap_err()
            .1,
            SendError::Unroutable
        );
    }
}
