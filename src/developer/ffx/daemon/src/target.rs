// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::constants::{MDNS_BROADCAST_INTERVAL_SECS, MDNS_TARGET_DROP_GRACE_PERIOD_SECS},
    crate::events::{self, DaemonEvent, EventSynthesizer},
    crate::fastboot::open_interface_with_serial,
    crate::logger::{streamer::DiagnosticsStreamer, Logger},
    crate::net::IsLocalAddr,
    crate::onet::HostPipeConnection,
    crate::task::{SingleFlight, TaskSnapshot},
    anyhow::{anyhow, Context, Error, Result},
    async_std::sync::RwLock,
    async_trait::async_trait,
    chrono::{DateTime, Utc},
    fidl::endpoints::ServiceMarker,
    fidl_fuchsia_developer_bridge as bridge,
    fidl_fuchsia_developer_remotecontrol::{
        IdentifyHostError, RemoteControlMarker, RemoteControlProxy,
    },
    fidl_fuchsia_net::{IpAddress, Ipv4Address, Ipv6Address, Subnet},
    fidl_fuchsia_overnet::ServiceConsumerProxyInterface,
    fidl_fuchsia_overnet_protocol::NodeId,
    fuchsia_async::Timer,
    futures::future,
    futures::lock::Mutex,
    futures::prelude::*,
    std::cmp::Ordering,
    std::collections::{BTreeSet, HashMap},
    std::default::Default,
    std::fmt,
    std::fmt::{Debug, Display},
    std::hash::{Hash, Hasher},
    std::io::Write,
    std::net::{IpAddr, SocketAddr, SocketAddrV4, SocketAddrV6},
    std::sync::{Arc, Weak},
    std::time::Duration,
    usb_bulk::Interface,
};

pub use crate::target_task::*;

pub trait SshFormatter {
    fn ssh_fmt<W: Write>(&self, f: &mut W) -> std::io::Result<()>;
}

impl SshFormatter for TargetAddr {
    fn ssh_fmt<W: Write>(&self, f: &mut W) -> std::io::Result<()> {
        if self.ip.is_ipv6() {
            write!(f, "[")?;
        }
        write!(f, "{}", self.ip)?;
        if self.ip.is_ipv6() {
            if self.scope_id > 0 {
                write!(f, "%{}", self.scope_id)?;
            }
            write!(f, "]")?;
        }
        Ok(())
    }
}

/// A trait for returning a consistent SSH address.
///
/// Based on the structure from which the SSH address is coming, this will
/// return:
/// -- The first IPv4 address found, if there are only IPv4 addresses available
/// -- The first IPv6 address found, if these are available.
pub trait SshAddrFetcher {
    fn to_ssh_addr(self) -> Option<TargetAddr>;
}

macro_rules! assign_and_break_if_link_local {
    ($res:ident, $addr:ident $(,)?) => {
        $res = Some($addr.clone());
        if $addr.ip().is_local_addr() {
            break;
        }
    };
}

impl<'a, T: Copy + IntoIterator<Item = &'a TargetAddr>> SshAddrFetcher for &'a T {
    fn to_ssh_addr(self) -> Option<TargetAddr> {
        let mut res: Option<TargetAddr> = None;
        for addr in self.into_iter() {
            match res {
                Some(res_addr) => match SocketAddr::from(addr) {
                    SocketAddr::V6(_) => match res_addr.ip {
                        IpAddr::V6(_) => {
                            // Only overwrite if this is a link-local addr, else
                            // keep the first one found.
                            if addr.ip().is_local_addr() {
                                assign_and_break_if_link_local!(res, addr);
                            }
                        }
                        IpAddr::V4(_) => {
                            assign_and_break_if_link_local!(res, addr);
                        }
                    },
                    SocketAddr::V4(_) => {}
                },
                None => {
                    assign_and_break_if_link_local!(res, addr);
                }
            }
        }
        res
    }
}

#[async_trait]
pub trait ToFidlTarget {
    async fn to_fidl_target(self) -> bridge::Target;
}

#[derive(Debug, Clone)]
pub struct RcsConnection {
    pub proxy: RemoteControlProxy,
    pub overnet_id: NodeId,
}

impl Hash for RcsConnection {
    fn hash<H>(&self, state: &mut H)
    where
        H: Hasher,
    {
        self.overnet_id.id.hash(state)
    }
}

impl PartialEq for RcsConnection {
    fn eq(&self, other: &Self) -> bool {
        self.overnet_id == other.overnet_id
    }
}

impl Eq for RcsConnection {}

#[derive(Debug, Hash, Clone, PartialEq, Eq)]
pub enum TargetEvent {
    RcsActivated,
    Rediscovered,

    /// LHS is previous state, RHS is current state.
    ConnectionStateChanged(ConnectionState, ConnectionState),
}

#[derive(Debug)]
pub enum RcsConnectionError {
    /// There is something wrong with the FIDL connection.
    FidlConnectionError(fidl::Error),
    /// There is an error from within Rcs itself.
    RemoteControlError(IdentifyHostError),

    /// There is an error with the output from Rcs.
    TargetError(Error),
}

impl Display for RcsConnectionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RcsConnectionError::FidlConnectionError(ferr) => {
                write!(f, "fidl connection error: {}", ferr)
            }
            RcsConnectionError::RemoteControlError(ierr) => write!(f, "internal error: {:?}", ierr),
            RcsConnectionError::TargetError(error) => write!(f, "general error: {}", error),
        }
    }
}

impl RcsConnection {
    pub async fn new(id: &mut NodeId) -> Result<Self> {
        let (s, p) = fidl::Channel::create().context("failed to create zx channel")?;
        let _result = RcsConnection::connect_to_service(id, s)?;
        let proxy = RemoteControlProxy::new(
            fidl::AsyncChannel::from_channel(p).context("failed to make async channel")?,
        );

        Ok(Self { proxy, overnet_id: id.clone() })
    }

    pub fn copy_to_channel(&mut self, channel: fidl::Channel) -> Result<()> {
        RcsConnection::connect_to_service(&mut self.overnet_id, channel)
    }

    fn connect_to_service(overnet_id: &mut NodeId, channel: fidl::Channel) -> Result<()> {
        let svc = hoist::connect_as_service_consumer()?;
        svc.connect_to_service(overnet_id, RemoteControlMarker::NAME, channel)
            .map_err(|e| anyhow!("Error connecting to Rcs: {}", e))
    }

    // For testing.
    #[cfg(test)]
    pub fn new_with_proxy(proxy: RemoteControlProxy, id: &NodeId) -> Self {
        Self { proxy, overnet_id: id.clone() }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum ConnectionState {
    Disconnected,
    /// Contains the last known ping from mDNS.
    Mdns(DateTime<Utc>),
    /// Contains an actual connection to RCS.
    Rcs(RcsConnection),
    // TODO(awdavies): Have fastboot in here.
    Fastboot,
}

impl Default for ConnectionState {
    fn default() -> Self {
        ConnectionState::Disconnected
    }
}

impl ConnectionState {
    fn is_connected(&self) -> bool {
        match self {
            Self::Disconnected => false,
            _ => true,
        }
    }

    fn take_rcs(&mut self) -> Option<RcsConnection> {
        if self.is_rcs() {
            match std::mem::replace(self, ConnectionState::Disconnected) {
                ConnectionState::Rcs(r) => Some(r),
                _ => None,
            }
        } else {
            None
        }
    }

    fn is_rcs(&self) -> bool {
        match self {
            ConnectionState::Rcs(_) => true,
            _ => false,
        }
    }
}

#[derive(Debug, Clone, Hash)]
pub(crate) struct TargetAddrEntry {
    addr: TargetAddr,
    timestamp: DateTime<Utc>,
}

impl PartialEq for TargetAddrEntry {
    fn eq(&self, other: &Self) -> bool {
        self.addr.eq(&other.addr)
    }
}

impl Eq for TargetAddrEntry {}

impl From<(TargetAddr, DateTime<Utc>)> for TargetAddrEntry {
    fn from(t: (TargetAddr, DateTime<Utc>)) -> Self {
        let (addr, timestamp) = t;
        Self { addr, timestamp }
    }
}

impl From<TargetAddr> for TargetAddrEntry {
    fn from(addr: TargetAddr) -> Self {
        Self { addr, timestamp: Utc::now() }
    }
}

impl Ord for TargetAddrEntry {
    fn cmp(&self, other: &Self) -> Ordering {
        self.addr.cmp(&other.addr)
    }
}

impl PartialOrd for TargetAddrEntry {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

#[derive(Debug, Default)]
#[cfg_attr(test, derive(Clone, PartialEq, Eq))]
pub struct TargetState {
    pub connection_state: ConnectionState,
}

struct TargetInner {
    nodename: String,
    state: Mutex<TargetState>,
    last_response: RwLock<DateTime<Utc>>,
    addrs: RwLock<BTreeSet<TargetAddrEntry>>,
    // used for Fastboot
    serial: RwLock<Option<String>>,
    boot_timestamp_nanos: RwLock<Option<u64>>,
    diagnostics_info: Arc<DiagnosticsStreamer>,
}

impl Default for TargetInner {
    fn default() -> Self {
        Self {
            nodename: Default::default(),
            last_response: RwLock::new(Utc::now()),
            state: Mutex::new(TargetState::default()),
            addrs: RwLock::new(BTreeSet::new()),
            serial: RwLock::new(None),
            boot_timestamp_nanos: RwLock::new(None),
            diagnostics_info: Arc::new(DiagnosticsStreamer::default()),
        }
    }
}

impl TargetInner {
    fn new(nodename: &str) -> Self {
        Self { nodename: nodename.to_string(), ..Default::default() }
    }

    pub fn new_with_boot_timestamp(nodename: &str, boot_timestamp_nanos: u64) -> Self {
        Self {
            nodename: nodename.to_string(),
            boot_timestamp_nanos: RwLock::new(Some(boot_timestamp_nanos)),
            ..Default::default()
        }
    }

    pub fn new_with_addrs(nodename: &str, addrs: BTreeSet<TargetAddr>) -> Self {
        Self {
            nodename: nodename.to_string(),
            addrs: RwLock::new(addrs.iter().map(|e| (*e, Utc::now()).into()).collect()),
            ..Default::default()
        }
    }

    pub fn new_with_serial(nodename: &str, serial: &str) -> Self {
        Self {
            nodename: nodename.to_string(),
            serial: RwLock::new(Some(serial.to_string())),
            ..Default::default()
        }
    }

    /// Dependency injection constructor so we can insert a fake time for
    /// testing.
    #[cfg(test)]
    pub fn new_with_time(nodename: &str, time: DateTime<Utc>) -> Self {
        Self {
            nodename: nodename.to_string(),
            last_response: RwLock::new(time),
            ..Default::default()
        }
    }
}

#[async_trait]
impl EventSynthesizer<TargetEvent> for TargetInner {
    async fn synthesize_events(&self) -> Vec<TargetEvent> {
        match self.state.lock().await.connection_state {
            ConnectionState::Rcs(_) => vec![TargetEvent::RcsActivated],
            _ => vec![],
        }
    }
}

#[derive(Clone)]
pub struct WeakTarget {
    pub events: events::Queue<TargetEvent>,
    inner: Weak<TargetInner>,
}

impl WeakTarget {
    /// attempts to upgrade to a target with a null task manager.
    pub fn upgrade(&self) -> Option<Target> {
        let inner = self.inner.upgrade()?;
        let events = self.events.clone();
        Some(Target {
            inner,
            events,
            task_manager: Arc::new(SingleFlight::new(|_| futures::future::ready(Ok(())).boxed())),
        })
    }
}

#[derive(Clone)]
pub struct Target {
    pub events: events::Queue<TargetEvent>,

    // TODO(awdavies): This shouldn't need to be behind an Arc<>, but for some
    // reason (probably something to do with the merge_insert function in the
    // TargetCollection struct?) this will drop all tasks immediately if this
    // isn't an Arc<>.
    pub task_manager: Arc<SingleFlight<TargetTaskType, Result<(), String>>>,

    inner: Arc<TargetInner>,
}

impl Target {
    async fn mdns_monitor_loop(weak_target: WeakTarget, limit: Duration) -> Result<(), String> {
        let limit = chrono::Duration::from_std(limit).map_err(|e| format!("{:?}", e))?;
        loop {
            if let Some(t) = weak_target.upgrade() {
                let nodename = t.nodename();
                t.update_connection_state(|s| match s {
                    ConnectionState::Mdns(ref time) => {
                        let now = Utc::now();
                        if now.signed_duration_since(*time) > limit {
                            log::debug!(
                                "dropping target '{}'. MDNS response older than {}",
                                nodename,
                                limit,
                            );
                            ConnectionState::Disconnected
                        } else {
                            s
                        }
                    }
                    _ => s,
                })
                .await;
                Timer::new(Duration::from_secs(1)).await;
            } else {
                log::debug!("parent target dropped. exiting");
                break;
            }
        }
        Ok(())
    }

    pub async fn is_connected(&self) -> bool {
        self.inner.state.lock().await.connection_state.is_connected()
    }

    pub fn downgrade(&self) -> WeakTarget {
        WeakTarget { events: self.events.clone(), inner: Arc::downgrade(&self.inner) }
    }

    fn from_inner(inner: Arc<TargetInner>) -> Self {
        let events = events::Queue::new(&inner);
        let weak_inner = Arc::downgrade(&inner);
        let weak_target = WeakTarget { inner: weak_inner, events: events.clone() };
        let task_manager = Arc::new(SingleFlight::new(move |t| match t {
            TargetTaskType::HostPipe => HostPipeConnection::new(weak_target.clone()).boxed(),
            TargetTaskType::MdnsMonitor => Target::mdns_monitor_loop(
                weak_target.clone(),
                Duration::from_secs(
                    MDNS_BROADCAST_INTERVAL_SECS + MDNS_TARGET_DROP_GRACE_PERIOD_SECS,
                ),
            )
            .boxed(),
            TargetTaskType::ProactiveLog => Logger::new(weak_target.clone()).start().boxed(),
        }));
        Self { inner, events, task_manager }
    }

    pub fn new(nodename: &str) -> Self {
        let inner = Arc::new(TargetInner::new(nodename));
        Self::from_inner(inner)
    }

    pub fn new_with_boot_timestamp(nodename: &str, boot_timestamp_nanos: u64) -> Self {
        let inner = Arc::new(TargetInner::new_with_boot_timestamp(nodename, boot_timestamp_nanos));
        Self::from_inner(inner)
    }

    pub fn new_with_addrs(nodename: &str, addrs: BTreeSet<TargetAddr>) -> Self {
        let inner = Arc::new(TargetInner::new_with_addrs(nodename, addrs));
        Self::from_inner(inner)
    }

    pub fn new_with_serial(nodename: &str, serial: &str) -> Self {
        let inner = Arc::new(TargetInner::new_with_serial(nodename, serial));
        Self::from_inner(inner)
    }

    /// Dependency injection constructor so we can insert a fake time for
    /// testing.
    #[cfg(test)]
    pub fn new_with_time(nodename: &str, time: DateTime<Utc>) -> Self {
        let inner = Arc::new(TargetInner::new_with_time(nodename, time));
        Self::from_inner(inner)
    }

    async fn rcs_state(&self) -> bridge::RemoteControlState {
        let loop_running = self.task_manager.task_snapshot(TargetTaskType::HostPipe).await
            == TaskSnapshot::Running;
        let state = self.inner.state.lock().await;
        match (loop_running, &state.connection_state) {
            (true, ConnectionState::Rcs(_)) => bridge::RemoteControlState::Up,
            (true, _) => bridge::RemoteControlState::Down,
            (_, _) => bridge::RemoteControlState::Unknown,
        }
    }

    pub fn nodename(&self) -> String {
        self.inner.nodename.clone()
    }

    pub async fn boot_timestamp_nanos(&self) -> Option<u64> {
        self.inner.boot_timestamp_nanos.read().await.clone()
    }

    pub async fn update_boot_timestamp(&self, ts: Option<u64>) {
        let mut inner_ts = self.inner.boot_timestamp_nanos.write().await;
        *inner_ts = ts;
    }

    pub fn stream_info(&self) -> Arc<DiagnosticsStreamer> {
        self.inner.diagnostics_info.clone()
    }

    /// Allows a client to atomically update the connection state based on what
    /// is returned from the predicate.
    ///
    /// For example, if the client wants to update the connection state to RCS
    /// connected, but only if the target is already disconnected, it would
    /// look like this:
    ///
    /// ```rust
    ///   let rcs_connection = ...;
    ///   target.update_connection_state(move |s| {
    ///     match s {
    ///       ConnectionState::Disconnected => ConnectionState::Rcs(rcs_connection),
    ///       _ => s
    ///     }
    ///   }).await;
    /// ```
    ///
    /// The client must always return the state, as this is swapped with the
    /// current target state in-place.
    ///
    /// If the state changes, this will push a `ConnectionStateChanged` event
    /// to the event queue.
    pub async fn update_connection_state<F>(&self, func: F)
    where
        F: FnOnce(ConnectionState) -> ConnectionState + Sized + Send,
    {
        let mut state = self.inner.state.lock().await;
        let former_state = state.connection_state.clone();
        let update =
            (func)(std::mem::replace(&mut state.connection_state, ConnectionState::Disconnected));
        state.connection_state = update;
        if former_state != state.connection_state {
            let _ = self
                .events
                .push(TargetEvent::ConnectionStateChanged(
                    former_state,
                    state.connection_state.clone(),
                ))
                .await;
        }
    }

    pub async fn rcs(&self) -> Option<RcsConnection> {
        match &self.inner.state.lock().await.connection_state {
            ConnectionState::Rcs(conn) => Some(conn.clone()),
            _ => None,
        }
    }

    pub async fn usb(&self) -> Option<Interface> {
        match self.inner.serial.read().await.as_ref() {
            Some(s) => open_interface_with_serial(s).ok(),
            None => None,
        }
    }

    pub async fn last_response(&self) -> DateTime<Utc> {
        self.inner.last_response.read().await.clone()
    }

    pub async fn addrs(&self) -> Vec<TargetAddr> {
        let mut addrs = self.inner.addrs.read().await.iter().cloned().collect::<Vec<_>>();
        addrs.sort_by(|a, b| b.timestamp.cmp(&a.timestamp));
        addrs.drain(..).map(|e| e.addr).collect()
    }

    #[cfg(test)]
    pub(crate) async fn addrs_insert(&self, t: TargetAddr) {
        self.inner.addrs.write().await.replace(t.into());
    }

    #[cfg(test)]
    pub(crate) async fn addrs_insert_entry(&self, t: TargetAddrEntry) {
        self.inner.addrs.write().await.replace(t);
    }

    async fn addrs_extend<T>(&self, iter: T)
    where
        T: IntoIterator<Item = TargetAddr>,
    {
        let now = Utc::now();
        let mut addrs = self.inner.addrs.write().await;
        // This is functionally the same as the regular extend function, instead
        // replacing each item. This is to ensure that the timestamps are
        // updated as they are seen.
        iter.into_iter().for_each(move |e| {
            let _ = addrs.replace((e, now.clone()).into());
        });
    }

    async fn update_last_response(&self, other: DateTime<Utc>) {
        let mut last_response = self.inner.last_response.write().await;
        if *last_response < other {
            *last_response = other;
        }
    }

    async fn overwrite_state(&self, mut other: TargetState) {
        let mut state = self.inner.state.lock().await;
        if let Some(rcs) = other.connection_state.take_rcs() {
            // Nots this so that we know to push an event.
            let rcs_activated = !state.connection_state.is_rcs();
            state.connection_state = ConnectionState::Rcs(rcs);
            if rcs_activated {
                self.events.push(TargetEvent::RcsActivated).await.unwrap_or_else(|err| {
                    log::warn!("unable to enqueue RCS activation event: {:#}", err)
                });
            }
        }
    }

    pub async fn from_rcs_connection(r: RcsConnection) -> Result<Self, RcsConnectionError> {
        let fidl_target = match r.proxy.identify_host().await {
            Ok(res) => match res {
                Ok(target) => target,
                Err(e) => return Err(RcsConnectionError::RemoteControlError(e)),
            },
            Err(e) => return Err(RcsConnectionError::FidlConnectionError(e)),
        };
        let nodename = fidl_target
            .nodename
            .ok_or(RcsConnectionError::TargetError(anyhow!("nodename required")))?;

        // TODO(awdavies): Merge target addresses once the scope_id is picked
        // up properly, else there will be duplicate link-local addresses that
        // aren't usable.
        let target = match fidl_target.boot_timestamp_nanos {
            Some(t) => Target::new_with_boot_timestamp(nodename.as_ref(), t),
            None => Target::new(nodename.as_ref()),
        };

        // Forces drop of target state mutex so that target can be returned.
        {
            let mut target_state = target.inner.state.lock().await;
            target_state.connection_state = ConnectionState::Rcs(r);
        }

        Ok(target)
    }

    pub async fn run_host_pipe(&self) {
        self.task_manager.spawn_detached(TargetTaskType::HostPipe).await
    }

    pub async fn run_mdns_monitor(&self) {
        self.task_manager.spawn_detached(TargetTaskType::MdnsMonitor).await;
    }

    pub async fn run_logger(&self) {
        self.task_manager.spawn_detached(TargetTaskType::ProactiveLog).await;
    }
}

#[async_trait]
impl EventSynthesizer<DaemonEvent> for Target {
    async fn synthesize_events(&self) -> Vec<DaemonEvent> {
        vec![DaemonEvent::NewTarget(self.nodename())]
    }
}

#[async_trait]
impl ToFidlTarget for Target {
    async fn to_fidl_target(self) -> bridge::Target {
        let (addrs, last_response, rcs_state) =
            futures::join!(self.addrs(), self.last_response(), self.rcs_state());

        bridge::Target {
            nodename: Some(self.nodename()),
            addresses: Some(addrs.iter().map(|a| a.into()).collect()),
            age_ms: Some(
                match Utc::now().signed_duration_since(last_response).num_milliseconds() {
                    dur if dur < 0 => {
                        log::trace!(
                            "negative duration encountered on target '{}': {}",
                            self.inner.nodename,
                            dur
                        );
                        0
                    }
                    dur => dur,
                } as u64,
            ),
            rcs_state: Some(rcs_state),

            // TODO(awdavies): Gather more information here when possible.
            target_type: Some(bridge::TargetType::Unknown),
            target_state: Some(bridge::TargetState::Unknown),
            ..bridge::Target::EMPTY
        }
    }
}

impl From<events::TargetInfo> for Target {
    fn from(mut t: events::TargetInfo) -> Self {
        if let Some(s) = t.serial {
            Self::new_with_serial(t.nodename.as_ref(), &s)
        } else {
            Self::new_with_addrs(t.nodename.as_ref(), t.addresses.drain(..).collect())
        }
    }
}

impl Debug for Target {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Target {{ {:?} }}", self.inner.nodename)
    }
}

// TODO(fxbug.dev/52733): Have `TargetAddr` support serial numbers.
#[derive(Hash, Clone, Debug, Copy, Eq, PartialEq)]
pub struct TargetAddr {
    ip: IpAddr,
    scope_id: u32,
}

impl Ord for TargetAddr {
    fn cmp(&self, other: &Self) -> Ordering {
        let this_socket = SocketAddr::from(self);
        let other_socket = SocketAddr::from(other);
        this_socket.cmp(&other_socket)
    }
}

impl PartialOrd for TargetAddr {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Into<bridge::TargetAddrInfo> for &TargetAddr {
    fn into(self) -> bridge::TargetAddrInfo {
        bridge::TargetAddrInfo::Ip(bridge::TargetIp {
            ip: match self.ip {
                IpAddr::V6(i) => IpAddress::Ipv6(Ipv6Address { addr: i.octets().into() }),
                IpAddr::V4(i) => IpAddress::Ipv4(Ipv4Address { addr: i.octets().into() }),
            },
            scope_id: self.scope_id,
        })
    }
}

impl Into<bridge::TargetAddrInfo> for TargetAddr {
    fn into(self) -> bridge::TargetAddrInfo {
        (&self).into()
    }
}

impl From<bridge::TargetAddrInfo> for TargetAddr {
    fn from(t: bridge::TargetAddrInfo) -> Self {
        (&t).into()
    }
}

impl From<&bridge::TargetAddrInfo> for TargetAddr {
    fn from(t: &bridge::TargetAddrInfo) -> Self {
        let (addr, scope): (IpAddr, u32) = match t {
            bridge::TargetAddrInfo::Ip(ip) => match ip.ip {
                IpAddress::Ipv6(Ipv6Address { addr }) => (addr.into(), ip.scope_id),
                IpAddress::Ipv4(Ipv4Address { addr }) => (addr.into(), ip.scope_id),
            },
            // TODO(fxbug.dev/52733): Add serial numbers.,
        };

        (addr, scope).into()
    }
}

impl From<Subnet> for TargetAddr {
    fn from(i: Subnet) -> Self {
        // TODO(awdavies): Figure out if it's possible to get the scope_id from
        // this address.
        match i.addr {
            IpAddress::Ipv4(ip4) => SocketAddr::from((ip4.addr, 0)).into(),
            IpAddress::Ipv6(ip6) => SocketAddr::from((ip6.addr, 0)).into(),
        }
    }
}

impl From<TargetAddr> for SocketAddr {
    fn from(t: TargetAddr) -> Self {
        Self::from(&t)
    }
}

impl From<&TargetAddr> for SocketAddr {
    fn from(t: &TargetAddr) -> Self {
        match t.ip {
            IpAddr::V6(addr) => SocketAddr::V6(SocketAddrV6::new(addr, 0, 0, t.scope_id)),
            IpAddr::V4(addr) => SocketAddr::V4(SocketAddrV4::new(addr, 0)),
        }
    }
}

impl From<(IpAddr, u32)> for TargetAddr {
    fn from(f: (IpAddr, u32)) -> Self {
        Self { ip: f.0, scope_id: f.1 }
    }
}

impl From<SocketAddr> for TargetAddr {
    fn from(s: SocketAddr) -> Self {
        Self {
            ip: s.ip(),
            scope_id: match s {
                SocketAddr::V6(addr) => addr.scope_id(),
                _ => 0,
            },
        }
    }
}

impl TargetAddr {
    pub fn scope_id(&self) -> u32 {
        self.scope_id
    }

    pub fn ip(&self) -> IpAddr {
        self.ip.clone()
    }
}

impl Display for TargetAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.ip())?;

        if self.ip.is_link_local_addr() && self.scope_id() > 0 {
            write!(f, "%{}", self.scope_id())?;
        }

        Ok(())
    }
}

pub enum TargetQuery {
    Nodename(String),
    Addr(TargetAddr),
    OvernetId(u64),
}

impl TargetQuery {
    pub async fn matches(&self, t: &Target) -> bool {
        match self {
            Self::Nodename(nodename) => *nodename == t.inner.nodename,
            Self::Addr(addr) => {
                let addrs = t.addrs().await;
                // Try to do the lookup if either the scope_id is non-zero (and
                // the IP address is IPv6 OR if the address is just IPv4 (as the
                // scope_id is always zero in this case).
                if addr.scope_id != 0 || addr.ip.is_ipv4() {
                    return addrs.contains(addr);
                }

                // Currently there's no way to parse an IP string w/ a scope_id,
                // so if we're at this stage the address is IPv6 and has been
                // probably parsed with a string (or it's a global IPv6 addr).
                for target_addr in addrs.iter() {
                    if target_addr.ip == addr.ip {
                        return true;
                    }
                }

                false
            }
            Self::OvernetId(id) => match &t.inner.state.lock().await.connection_state {
                ConnectionState::Rcs(rcs) => rcs.overnet_id.id == *id,
                _ => false,
            },
        }
    }
}

impl From<&str> for TargetQuery {
    fn from(s: &str) -> Self {
        String::from(s).into()
    }
}

impl From<String> for TargetQuery {
    fn from(s: String) -> Self {
        match s.parse::<IpAddr>() {
            Ok(a) => Self::Addr((a, 0).into()),
            Err(_) => Self::Nodename(s),
        }
    }
}

impl From<TargetAddr> for TargetQuery {
    fn from(t: TargetAddr) -> Self {
        Self::Addr(t)
    }
}

impl From<u64> for TargetQuery {
    fn from(id: u64) -> Self {
        Self::OvernetId(id)
    }
}

pub struct TargetCollection {
    inner: RwLock<HashMap<String, Target>>,
    events: RwLock<Option<events::Queue<DaemonEvent>>>,
}

#[async_trait]
impl EventSynthesizer<DaemonEvent> for TargetCollection {
    async fn synthesize_events(&self) -> Vec<DaemonEvent> {
        let collection = self.inner.read().await;
        // TODO(awdavies): This won't be accurate once a target is able to create
        // more than one event at a time.
        let mut res = Vec::with_capacity(collection.len());
        for target in collection.values() {
            res.extend(target.synthesize_events().await);
        }
        res
    }
}

impl TargetCollection {
    pub fn new() -> Self {
        Self { inner: RwLock::new(HashMap::new()), events: RwLock::new(None) }
    }

    pub async fn set_event_queue(&self, q: events::Queue<DaemonEvent>) {
        // This should be the only place a write lock is ever held.
        self.events
            .write()
            .then(move |mut e| {
                *e = Some(q);
                future::ready(())
            })
            .await;
    }

    pub async fn targets(&self) -> Vec<Target> {
        self.inner.read().await.values().map(|t| t.clone()).collect()
    }

    pub async fn merge_insert(&self, t: Target) -> Target {
        let mut inner = self.inner.write().await;
        // TODO(awdavies): better merging (using more indices for matching).
        match inner.get(&t.inner.nodename) {
            Some(to_update) => {
                log::trace!("attempting to merge info into target: {}", t.inner.nodename);
                futures::join!(
                    to_update.update_last_response(t.last_response().await),
                    to_update.addrs_extend(t.addrs().await),
                    to_update.update_boot_timestamp(t.boot_timestamp_nanos().await),
                    to_update.overwrite_state(TargetState {
                        connection_state: std::mem::replace(
                            &mut t.inner.state.lock().await.connection_state,
                            ConnectionState::Disconnected
                        ),
                    }),
                );
                to_update.events.push(TargetEvent::Rediscovered).await.unwrap_or_else(|err| {
                    log::warn!("unable to enqueue rediscovered event: {:#}", err)
                });
                to_update.clone()
            }
            None => {
                let t: Target = t.into();
                inner.insert(t.nodename(), t.clone());
                log::info!("New target: {}", t.nodename());
                if let Some(e) = self.events.read().await.as_ref() {
                    e.push(DaemonEvent::NewTarget(t.nodename()))
                        .await
                        .unwrap_or_else(|e| log::warn!("unable to push new target event: {}", e));
                }

                t
            }
        }
    }

    /// Attempts to get a target based off of the default. Returns an error if
    /// there is no target available (this doesn't await a new target).
    pub async fn get_default(&self, n: Option<String>) -> Result<Target> {
        let targets = self.inner.read().await;
        match (targets.len(), n) {
            (0, None) => Err(anyhow!("no targets connected - is your device plugged in?")),
            (1, None) => {
                let res = targets
                    .values()
                    .next()
                    .ok_or(anyhow!("no targets in cache"))
                    .map(|t| t.clone())?;
                log::debug!(
                    "No default target selected, returning only target - {:?}",
                    res.nodename(),
                );
                Ok(res)
            }
            (_, None) => {
                // n > 1 case (0 and 1 are covered, and this is an unsigned integer).
                Err(anyhow!("more than one target - specify a default target"))
            }
            (_, Some(nodename)) => {
                // TODO(fxb/66152) Use target query instead.
                targets
                    .iter()
                    .find_map(|(target_nodename, target)| {
                        if target_nodename.contains(&nodename) {
                            Some(target.clone())
                        } else {
                            None
                        }
                    })
                    .ok_or(anyhow!("no targets found"))
            }
        }
    }

    pub async fn get(&self, t: TargetQuery) -> Option<Target> {
        for target in self.inner.read().await.values() {
            if t.matches(target).await {
                return Some(target.clone());
            }
        }

        None
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        chrono::offset::TimeZone,
        fidl, fidl_fuchsia_developer_remotecontrol as rcs,
        futures::{channel::mpsc, executor::block_on},
        std::net::{Ipv4Addr, Ipv6Addr},
    };

    async fn clone_target(t: &Target) -> Target {
        let inner = Arc::new(TargetInner::clone(&t.inner));
        Target::from_inner(inner)
    }

    impl Clone for TargetInner {
        fn clone(&self) -> Self {
            Self {
                nodename: self.nodename.clone(),
                last_response: RwLock::new(block_on(self.last_response.read()).clone()),
                state: Mutex::new(block_on(self.state.lock()).clone()),
                addrs: RwLock::new(block_on(self.addrs.read()).clone()),
                serial: RwLock::new(block_on(self.serial.read()).clone()),
                boot_timestamp_nanos: RwLock::new(
                    block_on(self.boot_timestamp_nanos.read()).clone(),
                ),
                diagnostics_info: self.diagnostics_info.clone(),
            }
        }
    }

    fn fake_now() -> DateTime<Utc> {
        Utc.ymd(2014, 10, 31).and_hms(9, 10, 12)
    }

    fn fake_elapsed() -> DateTime<Utc> {
        Utc.ymd(2014, 11, 2).and_hms(13, 2, 1)
    }

    impl PartialEq for Target {
        fn eq(&self, o: &Target) -> bool {
            self.nodename() == o.nodename()
                && *block_on(self.inner.last_response.read())
                    == *block_on(o.inner.last_response.read())
                && block_on(self.addrs()) == block_on(o.addrs())
                && *block_on(self.inner.state.lock()) == *block_on(o.inner.state.lock())
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_collection_insert_new() {
        let tc = TargetCollection::new();
        let nodename = String::from("what");
        let t = Target::new_with_time(&nodename, fake_now());
        tc.merge_insert(clone_target(&t).await).await;
        assert_eq!(&tc.get(nodename.clone().into()).await.unwrap(), &t);
        match tc.get("oihaoih".into()).await {
            Some(_) => panic!("string lookup should return Nobne"),
            _ => (),
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_collection_merge() {
        let tc = TargetCollection::new();
        let nodename = String::from("bananas");
        let t1 = Target::new_with_time(&nodename, fake_now());
        let t2 = Target::new_with_time(&nodename, fake_elapsed());
        let a1 = IpAddr::V4(Ipv4Addr::new(192, 168, 1, 1));
        let a2 = IpAddr::V6(Ipv6Addr::new(
            0xfe80, 0xcafe, 0xf00d, 0xf000, 0xb412, 0xb455, 0x1337, 0xfeed,
        ));
        t1.addrs_insert((a1.clone(), 1).into()).await;
        t2.addrs_insert((a2.clone(), 1).into()).await;
        tc.merge_insert(clone_target(&t2).await).await;
        tc.merge_insert(clone_target(&t1).await).await;
        let merged_target = tc.get(nodename.clone().into()).await.unwrap();
        assert_ne!(&merged_target, &t1);
        assert_ne!(&merged_target, &t2);
        assert_eq!(merged_target.addrs().await.len(), 2);
        assert_eq!(*merged_target.inner.last_response.read().await, fake_elapsed());
        assert!(merged_target.addrs().await.contains(&(a1, 1).into()));
        assert!(merged_target.addrs().await.contains(&(a2, 1).into()));
    }

    fn setup_fake_remote_control_service(
        send_internal_error: bool,
        nodename_response: String,
    ) -> RemoteControlProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<RemoteControlMarker>().unwrap();

        fuchsia_async::Task::spawn(async move {
            while let Ok(Some(req)) = stream.try_next().await {
                match req {
                    rcs::RemoteControlRequest::IdentifyHost { responder } => {
                        if send_internal_error {
                            let _ = responder
                                .send(&mut Err(rcs::IdentifyHostError::ListInterfacesFailed))
                                .context("sending testing error response")
                                .unwrap();
                        } else {
                            let result: Vec<Subnet> = vec![Subnet {
                                addr: IpAddress::Ipv4(Ipv4Address { addr: [192, 168, 0, 1] }),
                                prefix_len: 24,
                            }];
                            let nodename = if nodename_response.len() == 0 {
                                None
                            } else {
                                Some(nodename_response.clone())
                            };
                            responder
                                .send(&mut Ok(rcs::IdentifyHostResponse {
                                    nodename,
                                    addresses: Some(result),
                                    ..rcs::IdentifyHostResponse::EMPTY
                                }))
                                .context("sending testing response")
                                .unwrap();
                        }
                    }
                    _ => assert!(false),
                }
            }
        })
        .detach();

        proxy
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_from_rcs_connection_internal_err() {
        // TODO(awdavies): Do some form of PartialEq implementation for
        // the RcsConnectionError enum to avoid the nested matches.
        let conn = RcsConnection::new_with_proxy(
            setup_fake_remote_control_service(true, "foo".to_owned()),
            &NodeId { id: 123 },
        );
        match Target::from_rcs_connection(conn).await {
            Ok(_) => assert!(false),
            Err(e) => match e {
                RcsConnectionError::RemoteControlError(rce) => match rce {
                    rcs::IdentifyHostError::ListInterfacesFailed => (),
                    _ => assert!(false),
                },
                _ => assert!(false),
            },
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_from_rcs_connection_nodename_none() {
        let conn = RcsConnection::new_with_proxy(
            setup_fake_remote_control_service(false, "".to_owned()),
            &NodeId { id: 123456 },
        );
        match Target::from_rcs_connection(conn).await {
            Ok(_) => assert!(false),
            Err(e) => match e {
                RcsConnectionError::TargetError(_) => (),
                _ => assert!(false),
            },
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_from_rcs_connection_no_err() {
        let conn = RcsConnection::new_with_proxy(
            setup_fake_remote_control_service(false, "foo".to_owned()),
            &NodeId { id: 1234 },
        );
        match Target::from_rcs_connection(conn).await {
            Ok(t) => {
                assert_eq!(t.nodename(), "foo".to_string());
                assert_eq!(t.rcs().await.unwrap().overnet_id.id, 1234u64);
                // For now there shouldn't be any addresses put in here, as
                // there's not a consistent way to convert them yet.
                assert_eq!(t.addrs().await.len(), 0);
            }
            Err(_) => assert!(false),
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_query_matches_nodename() {
        let query = TargetQuery::from("foo");
        let target = Arc::new(Target::new("foo"));
        assert!(query.matches(&target).await);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_by_overnet_id() {
        const ID: u64 = 12345;
        let conn = RcsConnection::new_with_proxy(
            setup_fake_remote_control_service(false, "foo".to_owned()),
            &NodeId { id: ID },
        );
        let t = Target::from_rcs_connection(conn).await.unwrap();
        let tc = TargetCollection::new();
        tc.merge_insert(clone_target(&t).await).await;
        assert_eq!(tc.get(ID.into()).await.unwrap(), t);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_by_addr() {
        let addr: TargetAddr = (IpAddr::from([192, 168, 0, 1]), 0).into();
        let t = Target::new("foo");
        t.addrs_insert(addr.clone()).await;
        let tc = TargetCollection::new();
        tc.merge_insert(clone_target(&t).await).await;
        assert_eq!(tc.get(addr.into()).await.unwrap(), t);
        assert_eq!(tc.get("192.168.0.1".into()).await.unwrap(), t);
        assert!(tc.get("fe80::dead:beef:beef:beef".into()).await.is_none());

        let addr: TargetAddr =
            (IpAddr::from([0xfe80, 0x0, 0x0, 0x0, 0xdead, 0xbeef, 0xbeef, 0xbeef]), 3).into();
        let t = Target::new("fooberdoober");
        t.addrs_insert(addr.clone()).await;
        tc.merge_insert(clone_target(&t).await).await;
        assert_eq!(tc.get("fe80::dead:beef:beef:beef".into()).await.unwrap(), t);
        assert_eq!(tc.get(addr.clone().into()).await.unwrap(), t);
        assert_eq!(tc.get("fooberdoober".into()).await.unwrap(), t);
    }

    // Most of this is now handled in `task.rs`
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_disconnect_multiple_invocations() {
        let t = Arc::new(Target::new("flabbadoobiedoo"));
        {
            let addr: TargetAddr = (IpAddr::from([192, 168, 0, 1]), 0).into();
            t.addrs_insert(addr).await;
        }
        // Assures multiple "simultaneous" invocations to start the target
        // doesn't put it into a bad state that would hang.
        let _: ((), (), ()) =
            futures::join!(t.run_host_pipe(), t.run_host_pipe(), t.run_host_pipe());
    }

    struct RcsStateTest {
        loop_started: bool,
        rcs_is_some: bool,
        expected: bridge::RemoteControlState,
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_rcs_states() {
        for test in vec![
            RcsStateTest {
                loop_started: true,
                rcs_is_some: false,
                expected: bridge::RemoteControlState::Down,
            },
            RcsStateTest {
                loop_started: true,
                rcs_is_some: true,
                expected: bridge::RemoteControlState::Up,
            },
            RcsStateTest {
                loop_started: false,
                rcs_is_some: true,
                expected: bridge::RemoteControlState::Unknown,
            },
            RcsStateTest {
                loop_started: false,
                rcs_is_some: false,
                expected: bridge::RemoteControlState::Unknown,
            },
        ] {
            let t = Target::new("schlabbadoo");
            let a2 = IpAddr::V6(Ipv6Addr::new(
                0xfe80, 0xcafe, 0xf00d, 0xf000, 0xb412, 0xb455, 0x1337, 0xfeed,
            ));
            t.addrs_insert((a2, 2).into()).await;
            if test.loop_started {
                t.run_host_pipe().await;
            }
            {
                *t.inner.state.lock().await = TargetState {
                    connection_state: if test.rcs_is_some {
                        ConnectionState::Rcs(RcsConnection::new_with_proxy(
                            setup_fake_remote_control_service(true, "foobiedoo".to_owned()),
                            &NodeId { id: 123 },
                        ))
                    } else {
                        ConnectionState::Disconnected
                    },
                };
            }
            assert_eq!(t.rcs_state().await, test.expected);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_to_fidl_target() {
        let t = Target::new("cragdune-the-impaler");
        let a1 = IpAddr::V4(Ipv4Addr::new(192, 168, 1, 1));
        let a2 = IpAddr::V6(Ipv6Addr::new(
            0xfe80, 0xcafe, 0xf00d, 0xf000, 0xb412, 0xb455, 0x1337, 0xfeed,
        ));
        t.addrs_insert((a1, 1).into()).await;
        t.addrs_insert((a2, 1).into()).await;

        let t_conv = t.clone().to_fidl_target().await;
        assert_eq!(t.nodename(), t_conv.nodename.unwrap().to_string());
        let addrs = t.addrs().await;
        let conv_addrs = t_conv.addresses.unwrap();
        assert_eq!(addrs.len(), conv_addrs.len());

        // Will crash if any addresses are missing.
        for address in conv_addrs {
            let address = TargetAddr::from(address);
            assert!(addrs.iter().any(|&a| a == address));
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_event_synthesis() {
        let t = Target::new("clopperdoop");
        let vec = t.synthesize_events().await;
        assert_eq!(vec.len(), 1);
        assert_eq!(&vec[0], &DaemonEvent::NewTarget("clopperdoop".to_string()));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_collection_event_synthesis() {
        let t = Target::new("clam-chowder-is-tasty");
        let t2 = Target::new("this-is-a-crunchy-falafel");
        let t3 = Target::new("i-should-probably-eat-lunch");
        let t4 = Target::new("i-should-probably-eat-lunch");

        let tc = TargetCollection::new();
        tc.merge_insert(t).await;
        tc.merge_insert(t2).await;
        tc.merge_insert(t3).await;
        tc.merge_insert(t4).await;

        let events = tc.synthesize_events().await;
        assert_eq!(events.len(), 3);
        assert!(events
            .iter()
            .any(|e| e == &events::DaemonEvent::NewTarget("clam-chowder-is-tasty".to_string())));
        assert!(
            events
                .iter()
                .any(|e| e
                    == &events::DaemonEvent::NewTarget("this-is-a-crunchy-falafel".to_string()))
        );
        assert!(events.iter().any(
            |e| e == &events::DaemonEvent::NewTarget("i-should-probably-eat-lunch".to_string())
        ));
    }

    struct EventPusher {
        got: mpsc::UnboundedSender<String>,
    }

    impl EventPusher {
        fn new() -> (Self, mpsc::UnboundedReceiver<String>) {
            let (got, rx) = mpsc::unbounded::<String>();
            (Self { got }, rx)
        }
    }

    #[async_trait]
    impl events::EventHandler<DaemonEvent> for EventPusher {
        async fn on_event(&self, event: DaemonEvent) -> Result<bool> {
            if let DaemonEvent::NewTarget(s) = event {
                self.got.unbounded_send(s).unwrap();
                Ok(false)
            } else {
                panic!("this should never receive any other kind of event");
            }
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_collection_events() {
        let t = Target::new("clam-chowder-is-tasty");
        let t2 = Target::new("this-is-a-crunchy-falafel");
        let t3 = Target::new("i-should-probably-eat-lunch");

        let tc = Arc::new(TargetCollection::new());
        let queue = events::Queue::new(&tc);
        let (handler, rx) = EventPusher::new();
        queue.add_handler(handler).await;
        tc.set_event_queue(queue).await;
        tc.merge_insert(t).await;
        tc.merge_insert(t2).await;
        tc.merge_insert(t3).await;
        let results = rx.take(3).collect::<Vec<_>>().await;
        assert!(results.iter().any(|e| e == &"clam-chowder-is-tasty".to_string()));
        assert!(results.iter().any(|e| e == &"this-is-a-crunchy-falafel".to_string()));
        assert!(results.iter().any(|e| e == &"i-should-probably-eat-lunch".to_string()));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_event_synthesis_wait() {
        let conn = RcsConnection::new_with_proxy(
            setup_fake_remote_control_service(false, "foo".to_owned()),
            &NodeId { id: 1234 },
        );
        let t = match Target::from_rcs_connection(conn).await {
            Ok(t) => {
                assert_eq!(t.nodename(), "foo".to_string());
                assert_eq!(t.rcs().await.unwrap().overnet_id.id, 1234u64);
                // For now there shouldn't be any addresses put in here, as
                // there's not a consistent way to convert them yet.
                assert_eq!(t.addrs().await.len(), 0);
                t
            }
            Err(_) => unimplemented!("this branch should never happen"),
        };
        // This will hang forever if no synthesis happens.
        t.events.wait_for(None, |e| e == TargetEvent::RcsActivated).await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_event_fire() {
        let t = Target::new("balaowihf");
        let conn = RcsConnection::new_with_proxy(
            setup_fake_remote_control_service(false, "balaowihf".to_owned()),
            &NodeId { id: 1234 },
        );

        let fut = t.events.wait_for(None, |e| e == TargetEvent::RcsActivated);
        let mut new_state = TargetState::default();
        new_state.connection_state = ConnectionState::Rcs(conn);
        // This is a bit of a race, so it's possible that state will be
        // updated before the wait_for invocation is registered with the
        // event queue, but either way this should succeed.
        let (res, ()) = futures::join!(fut, t.overwrite_state(new_state));
        res.unwrap();
    }

    #[test]
    fn test_to_ssh_addr() {
        let sockets = vec![
            SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 1), 0)),
            SocketAddr::V6(SocketAddrV6::new("f111::3".parse().unwrap(), 0, 0, 0)),
            SocketAddr::V6(SocketAddrV6::new("fe80::1".parse().unwrap(), 0, 0, 0)),
            SocketAddr::V6(SocketAddrV6::new("fe80::2".parse().unwrap(), 0, 0, 0)),
        ];
        let addrs = sockets.iter().map(|s| TargetAddr::from(*s)).collect::<Vec<_>>();
        assert_eq!((&addrs).to_ssh_addr(), Some(addrs[2]));

        let sockets = vec![
            SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 1), 0)),
            SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(129, 0, 0, 1), 0)),
        ];
        let addrs = sockets.iter().map(|s| TargetAddr::from(*s)).collect::<Vec<_>>();
        assert_eq!((&addrs).to_ssh_addr(), Some(addrs[0]));

        let addrs = Vec::<TargetAddr>::new();
        assert_eq!((&addrs).to_ssh_addr(), None);
    }

    #[test]
    fn test_ssh_formatting() {
        struct SshFormatTest {
            addr: TargetAddr,
            expect: &'static str,
        };
        let tests_pre = vec![
            (SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 1), 0)), "127.0.0.1"),
            (SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(129, 0, 0, 1), 0)), "129.0.0.1"),
            (SocketAddr::V6(SocketAddrV6::new("f111::3".parse().unwrap(), 0, 0, 0)), "[f111::3]"),
            (SocketAddr::V6(SocketAddrV6::new("fe80::1".parse().unwrap(), 0, 0, 0)), "[fe80::1]"),
            (SocketAddr::V6(SocketAddrV6::new("fe80::2".parse().unwrap(), 0, 0, 2)), "[fe80::2%2]"),
        ];
        let tests = tests_pre
            .iter()
            .map(|t| SshFormatTest { addr: TargetAddr::from(t.0), expect: t.1 })
            .collect::<Vec<_>>();
        for test in tests.iter() {
            let mut res = Vec::<u8>::new();
            test.addr.ssh_fmt(&mut res).unwrap();
            assert_eq!(std::str::from_utf8(&res[..]).unwrap(), test.expect);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_get_default() {
        let default = "clam-chowder-is-tasty";
        let t = Target::new(default);
        let t2 = Target::new("this-is-a-crunchy-falafel");
        let tc = TargetCollection::new();
        assert!(tc.get_default(None).await.is_err());
        tc.merge_insert(clone_target(&t).await).await;
        assert_eq!(tc.get_default(Some(default.to_string())).await.unwrap(), t);
        assert_eq!(tc.get_default(None).await.unwrap(), t);
        tc.merge_insert(t2).await;
        assert_eq!(tc.get_default(Some(default.to_string())).await.unwrap(), t);
        assert!(tc.get_default(None).await.is_err());
        assert!(tc.get_default(Some("not_in_here".to_owned())).await.is_err());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_get_default_matches_contains() {
        let default = "clam-chowder-is-tasty";
        let t = Target::new(default);
        let t2 = Target::new("this-is-a-crunchy-falafel");
        let tc = TargetCollection::new();
        assert!(tc.get_default(None).await.is_err());
        tc.merge_insert(clone_target(&t).await).await;
        assert_eq!(tc.get_default(Some(default.to_string())).await.unwrap(), t);
        assert_eq!(tc.get_default(None).await.unwrap(), t);
        tc.merge_insert(t2).await;
        assert_eq!(tc.get_default(Some(default.to_string())).await.unwrap(), t);
        assert!(tc.get_default(None).await.is_err());
        assert!(tc.get_default(Some("not_in_here".to_owned())).await.is_err());
        assert_eq!(tc.get_default(Some("clam".to_string())).await.unwrap(), t);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_update_connection_state() {
        let t = Target::new("have-you-seen-my-cat");
        let fake_time = Utc.yo(2017, 12).and_hms(1, 2, 3);
        let fake_time_clone = fake_time.clone();
        t.update_connection_state(move |s| {
            assert_eq!(s, ConnectionState::Disconnected);

            ConnectionState::Mdns(fake_time_clone)
        })
        .await;
        assert_eq!(ConnectionState::Mdns(fake_time), t.inner.state.lock().await.connection_state);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_mdns_set_disconnected() {
        let t = Target::new("yo-yo-ma-plays-that-cello-ya-hear");
        let now = Utc::now();
        t.update_connection_state(|s| {
            assert_eq!(s, ConnectionState::Disconnected);
            ConnectionState::Mdns(now)
        })
        .await;
        let events = t.events.clone();
        let _task = fuchsia_async::Task::local(async move {
            Target::mdns_monitor_loop(t.downgrade(), Duration::from_secs(2))
                .await
                .expect("mdns monitor loop failed")
        });
        events
            .wait_for(None, move |e| {
                e == TargetEvent::ConnectionStateChanged(
                    ConnectionState::Mdns(now),
                    ConnectionState::Disconnected,
                )
            })
            .await
            .unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_addresses_order_preserved() {
        let t = Target::new("this-is-a-target-i-guess");
        let addrs_pre = vec![
            SocketAddr::V6(SocketAddrV6::new("fe80::1".parse().unwrap(), 0, 0, 0)),
            SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 1), 0)),
            SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(129, 0, 0, 1), 0)),
            SocketAddr::V6(SocketAddrV6::new("f111::3".parse().unwrap(), 0, 0, 0)),
            SocketAddr::V6(SocketAddrV6::new("fe80::1".parse().unwrap(), 0, 0, 0)),
            SocketAddr::V6(SocketAddrV6::new("fe80::2".parse().unwrap(), 0, 0, 2)),
        ];
        let mut addrs_post = addrs_pre
            .iter()
            .cloned()
            .enumerate()
            .map(|(i, e)| {
                (TargetAddr::from(e), Utc.ymd(2014 + (i as i32), 10, 31).and_hms(9, 10, 12)).into()
            })
            .collect::<Vec<TargetAddrEntry>>();
        for a in addrs_post.iter().cloned() {
            t.addrs_insert_entry(a).await;
        }

        // Removes expected duplicate address. Should be marked as a duplicate
        // and also removed from the very beginning as a more-recent version
        // is added later.
        addrs_post.remove(0);
        // The order should be: last one inserted should show up first.
        addrs_post.reverse();
        assert_eq!(addrs_post.drain(..).map(|e| e.addr).collect::<Vec<_>>(), t.addrs().await);
    }
}
