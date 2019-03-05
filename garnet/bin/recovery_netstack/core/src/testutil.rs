// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Testing-related utilities.

use std::collections::{BTreeMap, HashMap};
use std::sync::Once;
use std::time::{Duration, Instant};

use byteorder::{ByteOrder, NativeEndian};
use log::debug;
use packet::{ParsablePacket, ParseBuffer};
use rand::SeedableRng;
use rand_xorshift::XorShiftRng;

use crate::device::ethernet::{EtherType, Mac};
use crate::device::{DeviceId, DeviceLayerEventDispatcher};
use crate::error::{ParseError, ParseResult};
use crate::ip::{
    Ip, IpAddr, IpAddress, IpExt, IpPacket, IpProto, IpSubnet, Ipv4Addr, Subnet, IPV6_MIN_MTU,
};
use crate::transport::udp::UdpEventDispatcher;
use crate::transport::TransportLayerEventDispatcher;
use crate::wire::ethernet::EthernetFrame;
use crate::wire::icmp::{IcmpMessage, IcmpPacket, IcmpParseArgs};
use crate::{handle_timeout, Context, EventDispatcher, TimerId};

/// Create a new deterministic RNG from a seed.
pub fn new_rng(mut seed: u64) -> XorShiftRng {
    if seed == 0 {
        // XorShiftRng can't take 0 seeds
        seed = 1;
    }
    let mut bytes = [0; 16];
    NativeEndian::write_u32(&mut bytes[0..4], seed as u32);
    NativeEndian::write_u32(&mut bytes[4..8], (seed >> 32) as u32);
    NativeEndian::write_u32(&mut bytes[8..12], seed as u32);
    NativeEndian::write_u32(&mut bytes[12..16], (seed >> 32) as u32);
    XorShiftRng::from_seed(bytes)
}

#[derive(Default, Debug)]
pub struct TestCounters {
    data: HashMap<String, usize>,
}

impl TestCounters {
    pub fn increment(&mut self, key: &str) {
        *(self.data.entry(key.to_string()).or_insert(0)) += 1;
    }

    pub fn get(&self, key: &str) -> &usize {
        self.data.get(key).unwrap_or(&0)
    }
}

/// log::Log implementation that uses stdout.
///
/// Useful when debugging tests.
struct Logger;

impl log::Log for Logger {
    fn enabled(&self, _metadata: &log::Metadata) -> bool {
        true
    }

    fn log(&self, record: &log::Record) {
        println!("{}", record.args())
    }

    fn flush(&self) {}
}

static LOGGER: Logger = Logger;

static LOGGER_ONCE: Once = Once::new();

/// Install a logger for tests.
///
/// Call this method at the beginning of the test for which logging is desired.  This function sets
/// global program state, so all tests that run after this function is called will use the logger.
pub fn set_logger_for_test() {
    // log::set_logger will panic if called multiple times; using a Once makes
    // set_logger_for_test idempotent
    LOGGER_ONCE.call_once(|| {
        log::set_logger(&LOGGER).unwrap();
        log::set_max_level(log::LevelFilter::Trace);
    })
}

/// Skip current (fake) time forward to trigger the next timer event.
///
/// Returns true if a timer was triggered, false if there were no timers waiting to be
/// triggered.
pub fn trigger_next_timer(ctx: &mut Context<DummyEventDispatcher>) -> bool {
    match ctx
        .dispatcher
        .timer_events
        .keys()
        .next()
        .map(|t| *t)
        .and_then(|t| ctx.dispatcher.timer_events.remove(&t).map(|id| (t, id)))
    {
        Some((t, id)) => {
            ctx.dispatcher.current_time = t;
            handle_timeout(ctx, id);
            true
        }
        None => false,
    }
}

/// Parse an ethernet frame.
///
/// `parse_ethernet_frame` parses an ethernet frame, returning the body along
/// with some important header fields.
pub fn parse_ethernet_frame(mut buf: &[u8]) -> ParseResult<(&[u8], Mac, Mac, Option<EtherType>)> {
    let frame = (&mut buf).parse::<EthernetFrame<_>>()?;
    let src_mac = frame.src_mac();
    let dst_mac = frame.dst_mac();
    let ethertype = frame.ethertype();
    Ok((buf, src_mac, dst_mac, ethertype))
}

/// Parse an IP packet.
///
/// `parse_ip_packet` parses an IP packet, returning the body along with some
/// important header fields.
pub fn parse_ip_packet<I: Ip>(mut buf: &[u8]) -> ParseResult<(&[u8], I::Addr, I::Addr, IpProto)> {
    let packet = (&mut buf).parse::<<I as IpExt<_>>::Packet>()?;
    let src_ip = packet.src_ip();
    let dst_ip = packet.dst_ip();
    let proto = packet.proto();
    // Because the packet type here is generic, Rust doesn't know that it
    // doesn't implement Drop, and so it doesn't know that it's safe to drop as
    // soon as it's no longer used and allow buf to no longer be borrowed on the
    // next line. It works fine in parse_ethernet_frame because EthernetFrame is
    // a concrete type which Rust knows doesn't implement Drop.
    std::mem::drop(packet);
    Ok((buf, src_ip, dst_ip, proto))
}

/// Parse an ICMP packet.
///
/// `parse_icmp_packet` parses an ICMP packet, returning the body along with
/// some important fields. Before returning, it invokes the callback `f` on the
/// parsed packet.
pub fn parse_icmp_packet<
    I: Ip,
    C,
    M: for<'a> IcmpMessage<I, &'a [u8], Code = C>,
    F: for<'a> Fn(&IcmpPacket<I, &'a [u8], M>),
>(
    mut buf: &[u8],
    src_ip: I::Addr,
    dst_ip: I::Addr,
    f: F,
) -> ParseResult<(M, C)>
where
    for<'a> IcmpPacket<I, &'a [u8], M>:
        ParsablePacket<&'a [u8], IcmpParseArgs<I::Addr>, Error = ParseError>,
{
    let packet =
        (&mut buf).parse_with::<_, IcmpPacket<I, _, M>>(IcmpParseArgs::new(src_ip, dst_ip))?;
    let message = packet.message().clone();
    let code = packet.code();
    f(&packet);
    Ok((message, code))
}

/// Parse an IP packet in an Ethernet frame.
///
/// `parse_ip_packet_in_ethernet_frame` parses an IP packet in an Ethernet
/// frame, returning the body of the IP packet along with some important fields
/// from both the IP and Ethernet headers.
pub fn parse_ip_packet_in_ethernet_frame<I: Ip>(
    buf: &[u8],
) -> ParseResult<(&[u8], Mac, Mac, I::Addr, I::Addr, IpProto)> {
    use crate::device::ethernet::EthernetIpExt;
    let (body, src_mac, dst_mac, ethertype) = parse_ethernet_frame(buf)?;
    if ethertype != Some(I::ETHER_TYPE) {
        debug!("unexpected ethertype: {:?}", ethertype);
        return Err(ParseError::NotExpected);
    }
    let (body, src_ip, dst_ip, proto) = parse_ip_packet::<I>(body)?;
    Ok((body, src_mac, dst_mac, src_ip, dst_ip, proto))
}

/// Parse an ICMP packet in an IP packet in an Ethernet frame.
///
/// `parse_icmp_packet_in_ip_packet_in_ethernet_frame` parses an ICMP packet in
/// an IP packet in an Ethernet frame, returning the message and code from the
/// ICMP packet along with some important fields from both the IP and Ethernet
/// headers. Before returning, it invokes the callback `f` on the parsed packet.
pub fn parse_icmp_packet_in_ip_packet_in_ethernet_frame<
    I: Ip,
    C,
    M: for<'a> IcmpMessage<I, &'a [u8], Code = C>,
    F: for<'a> Fn(&IcmpPacket<I, &'a [u8], M>),
>(
    mut buf: &[u8],
    f: F,
) -> ParseResult<(Mac, Mac, I::Addr, I::Addr, M, C)>
where
    for<'a> IcmpPacket<I, &'a [u8], M>:
        ParsablePacket<&'a [u8], IcmpParseArgs<I::Addr>, Error = ParseError>,
{
    use crate::wire::icmp::IcmpIpExt;

    let (mut body, src_mac, dst_mac, src_ip, dst_ip, proto) =
        parse_ip_packet_in_ethernet_frame::<I>(buf)?;
    if proto != I::IP_PROTO {
        debug!("unexpected IP protocol: {} (wanted {})", proto, I::IP_PROTO);
        return Err(ParseError::NotExpected);
    }
    let (message, code) = parse_icmp_packet(body, src_ip, dst_ip, f)?;
    Ok((src_mac, dst_mac, src_ip, dst_ip, message, code))
}

/// A configuration for a simple network.
///
/// `DummyEventDispatcherConfig` describes a simple network with two IPv4 hosts
/// - one remote and one local - both on the same Ethernet network.
pub struct DummyEventDispatcherConfig {
    /// The subnet of the local Ethernet network.
    pub subnet: Subnet<Ipv4Addr>,
    /// The IP address of our interface to the local network (must be in
    /// subnet).
    pub local_ip: Ipv4Addr,
    /// The MAC address of our interface to the local network.
    pub local_mac: Mac,
    /// The remote host's IP address (must be in subnet).
    pub remote_ip: Ipv4Addr,
    /// The remote host's MAC address.
    pub remote_mac: Mac,
}

/// A `DummyEventDispatcherConfig` with reasonable values.
pub const DUMMY_CONFIG: DummyEventDispatcherConfig = DummyEventDispatcherConfig {
    subnet: unsafe { Subnet::new_unchecked(Ipv4Addr::new([192, 168, 0, 0]), 16) },
    local_ip: Ipv4Addr::new([192, 168, 0, 1]),
    local_mac: Mac::new([0, 1, 2, 3, 4, 5]),
    remote_ip: Ipv4Addr::new([192, 168, 0, 2]),
    remote_mac: Mac::new([6, 7, 8, 9, 10, 11]),
};

/// A builder for `DummyEventDispatcher`s.
///
/// A `DummyEventDispatcherBuilder` is capable of storing the configuration of a
/// network stack including forwarding table entries, devices and their assigned
/// IP addresses, ARP table entries, etc. It can be built using `build`,
/// producing a `Context<DummyEventDispatcher>` with all of the appropriate
/// state configured.
#[derive(Clone, Default)]
pub struct DummyEventDispatcherBuilder {
    devices: Vec<(Mac, Option<(IpAddr, IpSubnet)>)>,
    arp_table_entries: Vec<(usize, Ipv4Addr, Mac)>,
    // usize refers to index into devices Vec
    device_routes: Vec<(IpSubnet, usize)>,
    routes: Vec<(IpSubnet, IpAddr)>,
}

impl DummyEventDispatcherBuilder {
    /// Construct a `DummyEventDispatcherBuilder` from a `DummyEventDispatcherConfig`.
    pub fn from_config(cfg: DummyEventDispatcherConfig) -> DummyEventDispatcherBuilder {
        assert!(cfg.subnet.contains(cfg.local_ip));
        assert!(cfg.subnet.contains(cfg.remote_ip));

        let mut builder = DummyEventDispatcherBuilder::default();
        builder.devices.push((cfg.local_mac, Some((cfg.local_ip.into(), cfg.subnet.into()))));
        builder.arp_table_entries.push((0, cfg.remote_ip, cfg.remote_mac));
        builder.device_routes.push((cfg.subnet.into(), 0));
        builder
    }

    /// Add a device.
    ///
    /// `add_device` returns a key which can be used to refer to the device in
    /// future calls to `add_arp_table_entry` and `add_device_route`.
    pub fn add_device(&mut self, mac: Mac) -> usize {
        let idx = self.devices.len();
        self.devices.push((mac, None));
        idx
    }

    /// Add a device with an associated IP address.
    ///
    /// `add_device_with_ip` is like `add_device`, except that it takes an
    /// associated IP address and subnet to assign to the device.
    pub fn add_device_with_ip<A: IpAddress>(
        &mut self,
        mac: Mac,
        ip: A,
        subnet: Subnet<A>,
    ) -> usize {
        let idx = self.devices.len();
        self.devices.push((mac, Some((ip.into(), subnet.into()))));
        idx
    }

    /// Add an ARP table entry for a device's ARP table.
    pub fn add_arp_table_entry(&mut self, device: usize, ip: Ipv4Addr, mac: Mac) {
        self.arp_table_entries.push((device, ip, mac));
    }

    /// Add a route to the forwarding table.
    pub fn add_route<A: IpAddress>(&mut self, subnet: Subnet<A>, next_hop: A) {
        self.routes.push((subnet.into(), next_hop.into()));
    }

    /// Add a device route to the forwarding table.
    pub fn add_device_route<A: IpAddress>(&mut self, subnet: Subnet<A>, device: usize) {
        self.device_routes.push((subnet.into(), device));
    }

    /// Build a `Context<DummyEventDispatcher>` from the present configuration.
    pub fn build(self) -> Context<DummyEventDispatcher> {
        let mut ctx = Context::default();

        let DummyEventDispatcherBuilder { devices, arp_table_entries, device_routes, routes } =
            self;
        let mut idx_to_device_id =
            HashMap::<_, _, std::collections::hash_map::RandomState>::default();
        for (idx, (mac, ip_subnet)) in devices.into_iter().enumerate() {
            let id = ctx.state().add_ethernet_device(mac, IPV6_MIN_MTU);
            idx_to_device_id.insert(idx, id);
            match ip_subnet {
                Some((IpAddr::V4(ip), IpSubnet::V4(subnet))) => {
                    crate::device::set_ip_addr(&mut ctx, id, ip, subnet)
                }
                Some((IpAddr::V6(ip), IpSubnet::V6(subnet))) => {
                    crate::device::set_ip_addr(&mut ctx, id, ip, subnet)
                }
                None => {}
                _ => unreachable!(),
            }
        }
        for (idx, ip, mac) in arp_table_entries {
            let device = *idx_to_device_id.get(&idx).unwrap();
            crate::device::ethernet::insert_arp_table_entry(&mut ctx, device.id(), ip, mac);
        }
        for (subnet, idx) in device_routes {
            let device = *idx_to_device_id.get(&idx).unwrap();
            match subnet {
                IpSubnet::V4(subnet) => crate::ip::add_device_route(&mut ctx, subnet, device),
                IpSubnet::V6(subnet) => crate::ip::add_device_route(&mut ctx, subnet, device),
            }
        }
        for (subnet, next_hop) in routes {
            match (subnet, next_hop) {
                (IpSubnet::V4(subnet), IpAddr::V4(next_hop)) => {
                    crate::ip::add_route(&mut ctx, subnet, next_hop)
                }
                (IpSubnet::V6(subnet), IpAddr::V6(next_hop)) => {
                    crate::ip::add_route(&mut ctx, subnet, next_hop)
                }
                _ => unreachable!(),
            }
        }

        ctx
    }
}

/// A dummy `EventDispatcher` used for testing.
///
/// A `DummyEventDispatcher` implements the `EventDispatcher` interface for
/// testing purposes. It provides facilities to inspect the history of what
/// events have been emitted to the system.
pub struct DummyEventDispatcher {
    frames_sent: Vec<(DeviceId, Vec<u8>)>,
    timer_events: BTreeMap<Instant, TimerId>,
    current_time: Instant,
}

impl DummyEventDispatcher {
    pub fn frames_sent(&self) -> &[(DeviceId, Vec<u8>)] {
        &self.frames_sent
    }

    /// Get an ordered list of all scheduled timer events
    pub fn timer_events<'a>(&'a self) -> impl Iterator<Item = (&'a Instant, &'a TimerId)> {
        self.timer_events.iter()
    }

    /// Get the current (fake) time
    pub fn current_time(self) -> Instant {
        self.current_time
    }
}

impl Default for DummyEventDispatcher {
    fn default() -> DummyEventDispatcher {
        DummyEventDispatcher {
            frames_sent: vec![],
            timer_events: BTreeMap::new(),
            current_time: Instant::now(),
        }
    }
}

impl UdpEventDispatcher for DummyEventDispatcher {
    type UdpConn = ();
    type UdpListener = ();
}

impl TransportLayerEventDispatcher for DummyEventDispatcher {}

impl DeviceLayerEventDispatcher for DummyEventDispatcher {
    fn send_frame(&mut self, device: DeviceId, frame: &[u8]) {
        self.frames_sent.push((device, frame.to_vec()));
    }
}

impl EventDispatcher for DummyEventDispatcher {
    fn schedule_timeout(&mut self, duration: Duration, id: TimerId) -> Option<Instant> {
        self.schedule_timeout_instant(self.current_time + duration, id)
    }

    fn schedule_timeout_instant(&mut self, time: Instant, id: TimerId) -> Option<Instant> {
        let ret = self.cancel_timeout(id);
        self.timer_events.insert(time, id);
        ret
    }

    fn cancel_timeout(&mut self, id: TimerId) -> Option<Instant> {
        // There is the invariant that there can only be one timer event per TimerId, so we only
        // need to remove at most one element from timer_events.

        match self.timer_events.iter().find_map(|(instant, event_timer_id)| {
            if *event_timer_id == id {
                Some(*instant)
            } else {
                None
            }
        }) {
            Some(instant) => {
                self.timer_events.remove(&instant);
                Some(instant)
            }
            None => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::ip::{Ipv4, Ipv4Addr, Ipv6, Ipv6Addr};
    use crate::wire::icmp::{IcmpDestUnreachable, IcmpEchoReply, Icmpv4DestUnreachableCode};

    #[test]
    fn test_parse_ethernet_frame() {
        use crate::wire::testdata::ARP_REQUEST;
        let (body, src_mac, dst_mac, ethertype) = parse_ethernet_frame(ARP_REQUEST).unwrap();
        assert_eq!(body, &ARP_REQUEST[14..]);
        assert_eq!(src_mac, Mac::new([20, 171, 197, 116, 32, 52]));
        assert_eq!(dst_mac, Mac::new([255, 255, 255, 255, 255, 255]));
        assert_eq!(ethertype, Some(EtherType::Arp));
    }

    #[test]
    fn test_parse_ip_packet() {
        use crate::wire::testdata::icmp_redirect::IP_PACKET_BYTES;
        let (body, src_ip, dst_ip, proto) = parse_ip_packet::<Ipv4>(IP_PACKET_BYTES).unwrap();
        assert_eq!(body, &IP_PACKET_BYTES[20..]);
        assert_eq!(src_ip, Ipv4Addr::new([10, 123, 0, 2]));
        assert_eq!(dst_ip, Ipv4Addr::new([10, 123, 0, 1]));
        assert_eq!(proto, IpProto::Icmp);

        use crate::wire::testdata::icmp_echo_v6::REQUEST_IP_PACKET_BYTES;
        let (body, src_ip, dst_ip, proto) =
            parse_ip_packet::<Ipv6>(REQUEST_IP_PACKET_BYTES).unwrap();
        assert_eq!(body, &REQUEST_IP_PACKET_BYTES[40..]);
        assert_eq!(src_ip, Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]));
        assert_eq!(dst_ip, Ipv6Addr::new([0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]));
        assert_eq!(proto, IpProto::Icmpv6);
    }

    #[test]
    fn test_parse_ip_packet_in_ethernet_frame() {
        use crate::wire::testdata::tls_client_hello::*;
        let (body, src_mac, dst_mac, src_ip, dst_ip, proto) =
            parse_ip_packet_in_ethernet_frame::<Ipv4>(ETHERNET_FRAME_BYTES).unwrap();
        assert_eq!(body, &(ETHERNET_FRAME_BYTES[ETHERNET_BODY_RANGE])[IP_BODY_RANGE]);
        assert_eq!(src_mac, ETHERNET_SRC_MAC);
        assert_eq!(dst_mac, ETHERNET_DST_MAC);
        assert_eq!(src_ip, IP_SRC_IP);
        assert_eq!(dst_ip, IP_DST_IP);
        assert_eq!(proto, IpProto::Tcp);
    }

    #[test]
    fn test_parse_icmp_packet() {
        set_logger_for_test();
        use crate::wire::testdata::icmp_dest_unreachable::*;
        let (body, ..) = parse_ip_packet::<Ipv4>(&IP_PACKET_BYTES).unwrap();
        let (_, code) = parse_icmp_packet::<Ipv4, _, IcmpDestUnreachable, _>(
            body,
            Ipv4Addr::new([172, 217, 6, 46]),
            Ipv4Addr::new([192, 168, 0, 105]),
            |_| {},
        )
        .unwrap();
        assert_eq!(code, Icmpv4DestUnreachableCode::DestHostUnreachable);
    }

    #[test]
    fn test_parse_icmp_packet_in_ip_packet_in_ethernet_frame() {
        set_logger_for_test();
        use crate::wire::testdata::icmp_echo_ethernet::*;
        let (src_mac, dst_mac, src_ip, dst_ip, _, _) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv4, _, IcmpEchoReply, _>(
                &REPLY_ETHERNET_FRAME_BYTES,
                |_| {},
            )
            .unwrap();
        assert_eq!(src_mac, Mac::new([0x50, 0xc7, 0xbf, 0x1d, 0xf4, 0xd2]));
        assert_eq!(dst_mac, Mac::new([0x8c, 0x85, 0x90, 0xc9, 0xc9, 0x00]));
        assert_eq!(src_ip, Ipv4Addr::new([172, 217, 6, 46]));
        assert_eq!(dst_ip, Ipv4Addr::new([192, 168, 0, 105]));
    }
}
