// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Custom error types for the netstack.

use net_types::ip::{Ip, IpAddress};
use net_types::MulticastAddress;
use thiserror::Error;

use crate::device::AddressError;
use crate::wire::icmp::IcmpIpTypes;

/// Results returned from many functions in the netstack.
pub type Result<T> = std::result::Result<T, NetstackError>;

/// Results returned from parsing functions in the netstack.
pub(crate) type ParseResult<T> = std::result::Result<T, ParseError>;

/// Results returned from IP packet parsing functions in the netstack.
pub(crate) type IpParseResult<I, T> = std::result::Result<T, IpParseError<I>>;

/// Top-level error type the netstack.
#[derive(Error, Debug, PartialEq)]
pub enum NetstackError {
    #[error("{}", _0)]
    /// Errors related to packet parsing.
    Parse(ParseError),

    /// Error when item already exists.
    #[error("Item already exists")]
    Exists,

    /// Error when item is not found.
    #[error("Item not found")]
    NotFound,

    /// Errors related to sending UDP frames/packets.
    #[error("{}", _0)]
    SendUdp(crate::transport::udp::SendError),

    /// Errors related to connections.
    #[error("{}", _0)]
    Connect(SocketError),

    /// Error when there is no route to an address.
    #[error("No route to address")]
    NoRoute,

    /// Error when a maximum transmission unit (MTU) is exceeded.
    #[error("MTU exceeded")]
    Mtu,
    // Add error types here as we add more to the stack
}

impl From<AddressError> for NetstackError {
    fn from(error: AddressError) -> Self {
        match error {
            AddressError::AlreadyExists => Self::Exists,
            AddressError::NotFound => Self::NotFound,
        }
    }
}

/// Error type for packet parsing.
#[derive(Error, Debug, PartialEq)]
pub enum ParseError {
    /// Operation is not supported.
    #[error("Operation is not supported")]
    NotSupported,
    /// Operation is not expected in this context.
    #[error("Operation is not expected in this context")]
    NotExpected,
    /// Checksum is invalid.
    #[error("Invalid checksum")]
    Checksum,
    /// Packet is not formatted properly.
    #[error("Packet is not formatted properly")]
    Format,
}

/// Action to take when an error is encountered while parsing an IP packet.
#[derive(Copy, Clone, Debug, PartialEq)]
pub enum IpParseErrorAction {
    /// Discard the packet and do nothing further.
    DiscardPacket,

    /// Discard the packet and send an ICMP response.
    DiscardPacketSendICMP,

    /// Discard the packet and send an ICMP response if the packet's
    /// destination address was not a multicast address.
    DiscardPacketSendICMPNoMulticast,
}

impl IpParseErrorAction {
    /// Determines whether or not an ICMP message should be sent.
    ///
    /// Returns `true` if we should send an ICMP response. We should send
    /// an ICMP response if an action is set to `DiscardPacketSendICMP`, or
    /// if an action is set to `DiscardPacketSendICMPNoMulticast` and `dst_addr`
    /// (the destination address of the original packet that lead to a parsing
    /// error) is not a multicast address.
    pub(crate) fn should_send_icmp<A: IpAddress>(&self, dst_addr: &A) -> bool {
        match *self {
            IpParseErrorAction::DiscardPacket => false,
            IpParseErrorAction::DiscardPacketSendICMP => true,
            IpParseErrorAction::DiscardPacketSendICMPNoMulticast => !dst_addr.is_multicast(),
        }
    }

    /// Determines whether or not an ICMP message should be sent even if
    /// the original packet's destination address is a multicast.
    pub(crate) fn should_send_icmp_to_multicast(&self) -> bool {
        match *self {
            IpParseErrorAction::DiscardPacketSendICMP => true,
            IpParseErrorAction::DiscardPacket
            | IpParseErrorAction::DiscardPacketSendICMPNoMulticast => false,
        }
    }
}

/// Error type for IP packet parsing.
#[derive(Error, Debug, PartialEq)]
pub(crate) enum IpParseError<I: IcmpIpTypes> {
    #[error("Parsing Error")]
    Parse { error: ParseError },
    /// For errors where an ICMP Parameter Problem error needs to be
    /// sent to the source of a packet.
    ///
    /// `src_ip` and `dst_ip` are the source and destination IP addresses of the
    /// original packet. `header_len` is the length of the header that we know
    /// about up to the point of the parameter problem error. `action` is the
    /// action we should take after encountering the error. If `must_send_icmp`
    /// is `true` we MUST send an ICMP response if `action` specifies it;
    /// otherwise, we MAY choose to just discard the packet and do nothing
    /// further. `code` is the ICMP (ICMPv4 or ICMPv6) specific parameter
    /// problem code that provides more granular information about the parameter
    /// problem encountered. `pointer` is the offset of the erroneous value within
    /// the IP packet, calculated from the beginning of the IP packet.
    #[error("Parameter Problem")]
    ParameterProblem {
        src_ip: I::Addr,
        dst_ip: I::Addr,
        code: I::ParameterProblemCode,
        pointer: I::ParameterProblemPointer,
        must_send_icmp: bool,
        header_len: I::HeaderLen,
        action: IpParseErrorAction,
    },
}

impl<I: Ip> From<ParseError> for IpParseError<I> {
    fn from(error: ParseError) -> Self {
        IpParseError::Parse { error }
    }
}

/// Error when something exists unexpectedly, such as trying to add an
/// element when the element is already present.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct ExistsError;

impl From<ExistsError> for NetstackError {
    fn from(_: ExistsError) -> NetstackError {
        NetstackError::Exists
    }
}

impl From<ExistsError> for SocketError {
    fn from(_: ExistsError) -> SocketError {
        SocketError::Local(LocalAddressError::AddressInUse)
    }
}

/// Error when something unexpectedly doesn't exist, such as trying to
/// remove an element when the element is not present.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct NotFoundError;

impl From<NotFoundError> for NetstackError {
    fn from(_: NotFoundError) -> NetstackError {
        NetstackError::NotFound
    }
}

/// Error type for errors common to local addresses.
#[derive(Error, Debug, PartialEq)]
pub enum LocalAddressError {
    /// Cannot bind to address.
    #[error("can't bind to address")]
    CannotBindToAddress,

    /// Failed to allocate local port.
    #[error("failed to allocate local port")]
    FailedToAllocateLocalPort,

    /// Specified local address does not match any expected address.
    #[error("specified local address does not match any expected address")]
    AddressMismatch,

    /// The requested address/socket pair is in use.
    #[error("Address in use")]
    AddressInUse,
}

// TODO(joshlf): Once we support a more general model of sockets in which UDP and ICMP
// connections are special cases of UDP and ICMP sockets, we can introduce a more
// specialized ListenerError which does not contain the NoRoute variant.

/// An error encountered when attempting to create a UDP, TCP, or ICMP connection.
#[derive(Error, Debug, PartialEq)]
pub enum RemoteAddressError {
    /// No route to host.
    #[error("no route to host")]
    NoRoute,
}

/// Error type for connection errors.
#[derive(Error, Debug, PartialEq)]
pub enum SocketError {
    #[error("{}", _0)]
    /// Errors related to the local address.
    Local(LocalAddressError),

    #[error("{}", _0)]
    /// Errors related to the remote address.
    Remote(RemoteAddressError),
}

/// Error when no route exists to a remote address.
#[derive(Debug, PartialEq, Eq)]
pub struct NoRouteError;

impl From<NoRouteError> for NetstackError {
    fn from(_: NoRouteError) -> NetstackError {
        NetstackError::NoRoute
    }
}

impl From<NoRouteError> for SocketError {
    fn from(_: NoRouteError) -> SocketError {
        SocketError::Remote(RemoteAddressError::NoRoute)
    }
}
