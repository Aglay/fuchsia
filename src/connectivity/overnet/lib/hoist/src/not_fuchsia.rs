// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(not(target_os = "fuchsia"))]

use {
    failure::Error,
    fidl::endpoints::ClientEnd,
    fidl::Handle,
    fidl_fuchsia_overnet::{Peer, ServiceProviderMarker},
    fidl_fuchsia_overnet_protocol::StreamSocketGreeting,
    futures::prelude::*,
    overnet_core::{
        LinkId, Node, NodeId, NodeRuntime, RouterOptions, RouterTime, SendHandle, StreamDeframer,
        StreamFramer,
    },
    parking_lot::Mutex,
    std::cell::RefCell,
    std::rc::Rc,
    tokio::{io::AsyncRead, runtime::current_thread},
};

pub use fidl_fuchsia_overnet::OvernetProxyInterface;

pub const ASCENDD_CLIENT_CONNECTION_STRING: &str = "Lift me";
pub const ASCENDD_SERVER_CONNECTION_STRING: &str = "Yessir";
pub const DEFAULT_ASCENDD_PATH: &str = "/tmp/ascendd";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Overnet <-> API bindings

#[derive(Debug)]
enum OvernetCommand {
    ListPeers(u64, futures::channel::oneshot::Sender<(u64, Vec<Peer>)>),
    RegisterService(String, ClientEnd<ServiceProviderMarker>),
    ConnectToService(NodeId, String, fidl::Channel),
    AttachSocketLink(fidl::Socket, fidl_fuchsia_overnet::SocketLinkOptions),
}

struct Overnet {
    tx: Mutex<futures::channel::mpsc::UnboundedSender<OvernetCommand>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for Overnet {
    fn drop(&mut self) {
        self.tx.lock().close_channel();
        self.thread.take().unwrap().join().unwrap();
    }
}

impl Overnet {
    fn new() -> Overnet {
        let (tx, rx) = futures::channel::mpsc::unbounded();
        let thread = Some(std::thread::spawn(move || run_overnet(rx)));
        let tx = Mutex::new(tx);
        Overnet { tx, thread }
    }

    fn send(&self, cmd: OvernetCommand) {
        self.tx.lock().unbounded_send(cmd).unwrap();
    }
}

#[derive(PartialEq, Eq, PartialOrd, Ord, Clone, Copy, Debug)]
struct Time(std::time::Instant);

impl RouterTime for Time {
    type Duration = std::time::Duration;

    fn now() -> Self {
        Time(std::time::Instant::now())
    }

    fn after(t: Self, dt: Self::Duration) -> Self {
        Time(t.0 + dt)
    }
}

enum WriteState {
    Idle { link: tokio::io::WriteHalf<tokio::net::UnixStream> },
    Writing,
}

impl WriteState {
    fn become_writing(&mut self) -> Option<tokio::io::WriteHalf<tokio::net::UnixStream>> {
        match std::mem::replace(self, WriteState::Writing) {
            WriteState::Writing => None,
            WriteState::Idle { link } => Some(link),
        }
    }
}

struct Writer {
    write_state: WriteState,
    framer: StreamFramer,
}

struct OvernetRuntime {
    writer: Rc<RefCell<Writer>>,
    router_link_id: LinkId,
}

fn spawn_local(future: impl Future<Output = ()> + 'static) {
    current_thread::spawn(future.unit_error().boxed_local().compat());
}

impl NodeRuntime for OvernetRuntime {
    type Time = Time;
    type LinkId = ();

    fn handle_type(hdl: &Handle) -> Result<SendHandle, Error> {
        Ok(match hdl.handle_type() {
            fidl::FidlHdlType::Channel => SendHandle::Channel,
            fidl::FidlHdlType::Socket => unimplemented!(),
            fidl::FidlHdlType::Invalid => failure::bail!("Invalid handle"),
        })
    }

    fn spawn_local<F>(&mut self, future: F)
    where
        F: Future<Output = ()> + 'static,
    {
        spawn_local(future)
    }

    fn at(&mut self, t: Time, f: impl FnOnce() + 'static) {
        spawn_local(async move {
            futures::compat::Compat01As03::new(tokio::timer::Delay::new(t.0)).await.unwrap();
            f();
        })
    }

    fn router_link_id(&self, _id: Self::LinkId) -> LinkId {
        self.router_link_id
    }

    fn send_on_link(&mut self, _id: Self::LinkId, packet: &mut [u8]) -> Result<(), Error> {
        let mut writer = self.writer.borrow_mut();
        writer.framer.queue_send(packet)?;
        if let Some(tx) = writer.write_state.become_writing() {
            start_writes(self.writer.clone(), tx, writer.framer.take_sends());
        }
        Ok(())
    }
}

fn start_writes(
    writer: Rc<RefCell<Writer>>,
    tx: tokio::io::WriteHalf<tokio::net::UnixStream>,
    bytes: Vec<u8>,
) {
    if bytes.len() == 0 {
        writer.borrow_mut().write_state = WriteState::Idle { link: tx };
        return;
    }
    let wr = tokio::io::write_all(tx, bytes);
    let wr = futures::compat::Compat01As03::new(wr);
    spawn_local(finish_writes(writer, wr));
}

async fn finish_writes(
    writer: Rc<RefCell<Writer>>,
    wr: impl Future<
        Output = Result<(tokio::io::WriteHalf<tokio::net::UnixStream>, Vec<u8>), std::io::Error>,
    >,
) {
    match wr.await {
        Ok((tx, _)) => {
            let bytes = writer.borrow_mut().framer.take_sends();
            start_writes(writer, tx, bytes);
        }
        Err(e) => eprintln!("Write failed: {}", e),
    }
}

async fn run_list_peers_inner(
    node: Node<OvernetRuntime>,
    last_seen_version: u64,
    responder: futures::channel::oneshot::Sender<(u64, Vec<Peer>)>,
) -> Result<(), Error> {
    if let Err(_) = responder.send(node.list_peers(last_seen_version).await?) {
        println!("List peers stopped listening");
    }
    Ok(())
}

async fn run_list_peers(
    node: Node<OvernetRuntime>,
    last_seen_version: u64,
    responder: futures::channel::oneshot::Sender<(u64, Vec<Peer>)>,
) {
    if let Err(e) = run_list_peers_inner(node, last_seen_version, responder).await {
        println!("List peers gets error: {:?}", e);
    }
}

async fn read_incoming_inner(
    stream: tokio::io::ReadHalf<tokio::net::UnixStream>,
    mut chan: futures::channel::mpsc::Sender<Vec<u8>>,
) -> Result<(), Error> {
    let mut buf = [0u8; 1024];
    let mut stream = Some(stream);
    let mut deframer = StreamDeframer::new();

    loop {
        let rd = tokio::io::read(stream.take().unwrap(), &mut buf[..]);
        let rd = futures::compat::Compat01As03::new(rd);
        let (returned_stream, _, n) = rd.await?;
        if n == 0 {
            return Ok(());
        }
        stream = Some(returned_stream);
        deframer.queue_recv(&mut buf[..n]);
        while let Some(frame) = deframer.next_incoming_frame() {
            chan.send(frame).await?;
        }
    }
}

async fn read_incoming(
    stream: tokio::io::ReadHalf<tokio::net::UnixStream>,
    chan: futures::channel::mpsc::Sender<Vec<u8>>,
) {
    if let Err(e) = read_incoming_inner(stream, chan).await {
        eprintln!("Error reading: {}", e);
    }
}

async fn process_incoming_inner(
    node: Node<OvernetRuntime>,
    mut rx_frames: futures::channel::mpsc::Receiver<Vec<u8>>,
    link_id: LinkId,
) -> Result<(), Error> {
    while let Some(mut frame) = rx_frames.next().await {
        node.queue_recv(link_id, frame.as_mut())?;
    }
    Ok(())
}

async fn process_incoming(
    node: Node<OvernetRuntime>,
    rx_frames: futures::channel::mpsc::Receiver<Vec<u8>>,
    link_id: LinkId,
) {
    if let Err(e) = process_incoming_inner(node, rx_frames, link_id).await {
        eprintln!("Error processing incoming frames: {}", e);
    }
}

async fn run_overnet_prelude() -> Result<Node<OvernetRuntime>, Error> {
    let node_id = overnet_core::generate_node_id();
    log::trace!("Hoist node id:  {}", node_id.0);

    let ascendd_path = std::env::var("ASCENDD").unwrap_or(DEFAULT_ASCENDD_PATH.to_string());
    let connection_label = std::env::var("OVERNET_CONNECTION_LABEL").ok();

    log::trace!("Ascendd path: {}", ascendd_path);
    log::trace!("Overnet connection label: {:?}", connection_label);
    let uds = tokio::net::UnixStream::connect(ascendd_path);
    let uds = futures::compat::Compat01As03::new(uds);
    let (rx_bytes, tx_bytes) = uds.await?.split();
    let (tx_frames, mut rx_frames) = futures::channel::mpsc::channel(8);

    spawn_local(read_incoming(rx_bytes, tx_frames));

    // Send first frame
    let mut framer = StreamFramer::new();
    let mut greeting = StreamSocketGreeting {
        magic_string: Some(ASCENDD_CLIENT_CONNECTION_STRING.to_string()),
        node_id: Some(fidl_fuchsia_overnet_protocol::NodeId { id: node_id.0 }),
        connection_label,
    };
    let mut bytes = Vec::new();
    let mut handles = Vec::new();
    fidl::encoding::Encoder::encode(&mut bytes, &mut handles, &mut greeting)?;
    assert_eq!(handles.len(), 0);
    framer.queue_send(bytes.as_slice())?;
    let send = framer.take_sends();
    let wr = tokio::io::write_all(tx_bytes, send);
    let wr = futures::compat::Compat01As03::new(wr).map_err(|e| -> Error { e.into() });

    log::trace!("Wait for greeting & first frame write");
    let first_frame = rx_frames
        .next()
        .map(|r| r.ok_or_else(|| failure::format_err!("Stream closed before greeting received")));
    let (mut frame, (tx_bytes, _)) = futures::try_join!(first_frame, wr)?;

    let mut greeting = StreamSocketGreeting::empty();
    fidl::encoding::Decoder::decode_into(frame.as_mut(), &mut [], &mut greeting)?;

    log::trace!("Got greeting: {:?}", greeting);
    let ascendd_node_id = match greeting {
        StreamSocketGreeting { magic_string: None, .. } => failure::bail!(
            "Required magic string '{}' not present in greeting",
            ASCENDD_SERVER_CONNECTION_STRING
        ),
        StreamSocketGreeting { magic_string: Some(ref x), .. }
            if x != ASCENDD_SERVER_CONNECTION_STRING =>
        {
            failure::bail!(
                "Expected magic string '{}' in greeting, got '{}'",
                ASCENDD_SERVER_CONNECTION_STRING,
                x
            )
        }
        StreamSocketGreeting { node_id: None, .. } => failure::bail!("No node id in greeting"),
        StreamSocketGreeting { node_id: Some(n), .. } => n.id,
    };

    let mut node = Node::new(
        OvernetRuntime {
            writer: Rc::new(RefCell::new(Writer {
                write_state: WriteState::Idle { link: tx_bytes },
                framer,
            })),
            router_link_id: LinkId::invalid(),
        },
        RouterOptions::new()
            .set_node_id(node_id)
            .set_quic_server_key_file(hard_coded_server_key()?)
            .set_quic_server_cert_file(hard_coded_server_cert()?),
    );
    let ascendd_link_id = node.new_link(ascendd_node_id.into(), ())?;
    node.with_runtime_mut(|rt| rt.router_link_id = ascendd_link_id);

    spawn_local(process_incoming(node.clone(), rx_frames, ascendd_link_id));

    Ok(node)
}

async fn run_overnet_inner(
    mut rx: futures::channel::mpsc::UnboundedReceiver<OvernetCommand>,
) -> Result<(), Error> {
    let node = run_overnet_prelude().await?;

    // Run application loop
    loop {
        let cmd = rx.next().await;
        log::trace!("cmd = {:?}", cmd);
        let r = match cmd {
            None => return Ok(()),
            Some(OvernetCommand::ListPeers(last_seen_version, sender)) => {
                spawn_local(run_list_peers(node.clone(), last_seen_version, sender));
                Ok(())
            }
            Some(OvernetCommand::RegisterService(service_name, provider)) => {
                node.register_service(service_name, provider)
            }
            Some(OvernetCommand::ConnectToService(node_id, service_name, channel)) => {
                node.connect_to_service(node_id, &service_name, channel)
            }
            Some(OvernetCommand::AttachSocketLink(socket, options)) => {
                node.attach_socket_link(options.connection_label, socket)
            }
        };
        if let Err(e) = r {
            log::warn!("cmd failed: {:?}", e);
        }
    }
}

fn run_overnet(rx: futures::channel::mpsc::UnboundedReceiver<OvernetCommand>) {
    current_thread::Runtime::new()
        .unwrap()
        .block_on(
            async move {
                if let Err(e) = run_overnet_inner(rx).await {
                    eprintln!("Main loop failed: {}", e);
                }
            }
                .unit_error()
                .boxed_local()
                .compat(),
        )
        .unwrap();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// OvernetProxyInterface implementation

impl OvernetProxyInterface for Overnet {
    type ListPeersResponseFut = futures::future::MapErr<
        futures::channel::oneshot::Receiver<(u64, Vec<Peer>)>,
        fn(futures::channel::oneshot::Canceled) -> fidl::Error,
    >;

    fn list_peers(&self, last_seen_version: u64) -> Self::ListPeersResponseFut {
        let (sender, receiver) = futures::channel::oneshot::channel();
        self.send(OvernetCommand::ListPeers(last_seen_version, sender));
        // Returning an error from the receiver means that the sender disappeared without
        // sending a response, a condition we explicitly disallow.
        receiver.map_err(|_| unreachable!())
    }

    fn register_service(
        &self,
        service_name: &str,
        provider: ClientEnd<ServiceProviderMarker>,
    ) -> Result<(), fidl::Error> {
        self.send(OvernetCommand::RegisterService(service_name.to_string(), provider));
        Ok(())
    }

    fn connect_to_service(
        &self,
        node: &mut fidl_fuchsia_overnet_protocol::NodeId,
        service_name: &str,
        chan: fidl::Channel,
    ) -> Result<(), fidl::Error> {
        self.send(OvernetCommand::ConnectToService(node.id.into(), service_name.to_string(), chan));
        Ok(())
    }

    fn attach_socket_link(
        &self,
        socket: fidl::Socket,
        options: fidl_fuchsia_overnet::SocketLinkOptions,
    ) -> Result<(), fidl::Error> {
        self.send(OvernetCommand::AttachSocketLink(socket, options));
        Ok(())
    }
}

/// Normal applications should call connect() to obtain the OvernetProxyInterface
pub fn connect() -> Result<impl OvernetProxyInterface, Error> {
    Ok(Overnet::new())
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Executor implementation

pub fn run(future: impl Future<Output = ()> + 'static) {
    crate::logger::init().unwrap();
    current_thread::Runtime::new()
        .unwrap()
        .block_on(future.unit_error().boxed_local().compat())
        .unwrap();
}

pub fn spawn<F>(future: F)
where
    F: Future<Output = ()> + 'static,
{
    current_thread::spawn(future.unit_error().boxed_local().compat());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Hacks to hardcode a resource file without resources

fn temp_file_containing(bytes: &[u8]) -> Result<Box<dyn AsRef<std::path::Path>>, Error> {
    let mut path = tempfile::NamedTempFile::new()?;
    use std::io::Write;
    path.write_all(bytes)?;
    Ok(Box::new(path))
}

pub fn hard_coded_server_cert() -> Result<Box<dyn AsRef<std::path::Path>>, Error> {
    temp_file_containing(include_bytes!(
        "../../../../../../third_party/rust-mirrors/quiche/examples/cert.crt"
    ))
}

pub fn hard_coded_server_key() -> Result<Box<dyn AsRef<std::path::Path>>, Error> {
    temp_file_containing(include_bytes!(
        "../../../../../../third_party/rust-mirrors/quiche/examples/cert.key"
    ))
}
