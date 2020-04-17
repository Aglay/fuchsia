// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(not(target_os = "fuchsia"))]

use {
    anyhow::{bail, ensure, Context as _, Error},
    fidl::endpoints::ClientEnd,
    fidl_fuchsia_overnet::{Peer, ServiceProviderMarker},
    fidl_fuchsia_overnet_protocol::StreamSocketGreeting,
    fuchsia_zircon_status as zx_status,
    futures::prelude::*,
    overnet_core::{
        new_deframer, new_framer, wait_until, DeframerWriter, FrameType, FramerReader,
        LosslessBinary, NodeId, Router, RouterOptions,
    },
    parking_lot::Mutex,
    std::{rc::Rc, sync::Arc, time::Instant},
    tokio::{io::AsyncRead, runtime::current_thread},
};

pub use fidl_fuchsia_overnet::MeshControllerProxyInterface;
pub use fidl_fuchsia_overnet::ServiceConsumerProxyInterface;
pub use fidl_fuchsia_overnet::ServicePublisherProxyInterface;

pub const ASCENDD_CLIENT_CONNECTION_STRING: &str = "ASCENDD_CLIENT_CONNECTION_STRING";
pub const ASCENDD_SERVER_CONNECTION_STRING: &str = "ASCENDD_SERVER_CONNECTION_STRING";
pub const DEFAULT_ASCENDD_PATH: &str = "/tmp/ascendd";

pub fn run<R>(f: impl Future<Output = R> + 'static) -> R {
    overnet_core::run(f)
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Overnet <-> API bindings

#[derive(Debug)]
enum OvernetCommand {
    ListPeers(futures::channel::oneshot::Sender<Vec<Peer>>),
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
    fn new() -> Result<Overnet, Error> {
        let (tx, rx) = futures::channel::mpsc::unbounded();
        let rx = Arc::new(Mutex::new(rx));
        let thread = Some(
            std::thread::Builder::new()
                .spawn(move || run_overnet(rx))
                .context("Spawning overnet thread")?,
        );
        let tx = Mutex::new(tx);
        Ok(Overnet { tx, thread })
    }

    fn send(&self, cmd: OvernetCommand) {
        self.tx.lock().unbounded_send(cmd).unwrap();
    }
}

#[derive(PartialEq, Eq, PartialOrd, Ord, Clone, Copy, Debug)]
struct Time(std::time::Instant);

async fn read_incoming(
    stream: tokio::io::ReadHalf<tokio::net::UnixStream>,
    mut incoming_writer: DeframerWriter<LosslessBinary>,
) -> Result<(), Error> {
    let mut buf = [0u8; 1024];
    let mut stream = Some(stream);

    loop {
        let rd = tokio::io::read(stream.take().unwrap(), &mut buf[..]);
        let rd = futures::compat::Compat01As03::new(rd);
        let (returned_stream, _, n) = rd.await?;
        if n == 0 {
            return Ok(());
        }
        stream = Some(returned_stream);
        incoming_writer.write(&buf[..n]).await?;
    }
}

async fn write_outgoing(
    mut outgoing_reader: FramerReader<LosslessBinary>,
    tx_bytes: tokio::io::WriteHalf<tokio::net::UnixStream>,
) -> Result<(), Error> {
    let mut tx_bytes = Some(tx_bytes);
    loop {
        let out = outgoing_reader.read().await?;
        let wr = tokio::io::write_all(tx_bytes.take().unwrap(), out.as_slice());
        let wr = futures::compat::Compat01As03::new(wr).map_err(|e| -> Error { e.into() });
        let (t, _) = wr.await?;
        tx_bytes = Some(t);
    }
}

async fn run_ascendd_connection(node: Rc<Router>) -> Result<(), Error> {
    let ascendd_path = std::env::var("ASCENDD").unwrap_or(DEFAULT_ASCENDD_PATH.to_string());
    let mut connection_label = std::env::var("OVERNET_CONNECTION_LABEL").ok();
    if connection_label.is_none() {
        connection_label = std::env::current_exe()
            .ok()
            .map(|p| format!("exe:{} pid:{}", p.display(), std::process::id()));
    }
    if connection_label.is_none() {
        connection_label = Some(format!("pid:{}", std::process::id()));
    }

    log::trace!("Ascendd path: {}", ascendd_path);
    log::trace!("Overnet connection label: {:?}", connection_label);
    let uds = tokio::net::UnixStream::connect(ascendd_path.clone());
    let uds = futures::compat::Compat01As03::new(uds);
    let uds = uds.await.context(format!("Opening uds path: {}", ascendd_path))?;
    let (rx_bytes, tx_bytes) = uds.split();
    let (mut framer, outgoing_reader) = new_framer(LosslessBinary, 4096);
    let (incoming_writer, mut deframer) = new_deframer(LosslessBinary);

    let _ = futures::future::try_join3(
        read_incoming(rx_bytes, incoming_writer),
        write_outgoing(outgoing_reader, tx_bytes),
        async move {
            // Send first frame
            let mut greeting = StreamSocketGreeting {
                magic_string: Some(ASCENDD_CLIENT_CONNECTION_STRING.to_string()),
                node_id: Some(fidl_fuchsia_overnet_protocol::NodeId { id: node.node_id().0 }),
                connection_label,
            };
            let mut bytes = Vec::new();
            let mut handles = Vec::new();
            fidl::encoding::Encoder::encode(&mut bytes, &mut handles, &mut greeting)?;
            assert_eq!(handles.len(), 0);
            framer.write(FrameType::Overnet, bytes.as_slice()).await?;

            log::info!("Wait for greeting & first frame write");
            let (frame_type, mut frame) = deframer.read().await?;
            ensure!(frame_type == Some(FrameType::Overnet), "Expect Overnet frame as first frame");

            let mut greeting = StreamSocketGreeting::empty();
            // WARNING: Since we are decoding without a transaction header, we have to
            // provide a context manually. This could cause problems in future FIDL wire
            // format migrations, which are driven by header flags.
            let context = fidl::encoding::Context {};
            fidl::encoding::Decoder::decode_with_context(
                &context,
                frame.as_mut(),
                &mut [],
                &mut greeting,
            )?;

            log::info!("Got greeting: {:?}", greeting);
            let ascendd_node_id = match greeting {
                StreamSocketGreeting { magic_string: None, .. } => bail!(
                    "Required magic string '{}' not present in greeting",
                    ASCENDD_SERVER_CONNECTION_STRING
                ),
                StreamSocketGreeting { magic_string: Some(ref x), .. }
                    if x != ASCENDD_SERVER_CONNECTION_STRING =>
                {
                    bail!(
                        "Expected magic string '{}' in greeting, got '{}'",
                        ASCENDD_SERVER_CONNECTION_STRING,
                        x
                    )
                }
                StreamSocketGreeting { node_id: None, .. } => bail!("No node id in greeting"),
                StreamSocketGreeting { node_id: Some(n), .. } => n.id,
            };

            let link = node.new_link(ascendd_node_id.into()).await?;

            let link_receiver = link.clone();
            let _: ((), ()) = futures::future::try_join(
                async move {
                    loop {
                        let (frame_type, mut frame) = deframer.read().await?;
                        ensure!(
                            frame_type == Some(FrameType::Overnet),
                            "Should only see Overnet frames"
                        );
                        if let Err(e) = link_receiver.received_packet(frame.as_mut_slice()).await {
                            log::warn!("Error receiving packet: {}", e);
                        }
                    }
                },
                async move {
                    let mut buffer = [0u8; 2048];
                    while let Some(n) = link.next_send(&mut buffer).await? {
                        framer.write(FrameType::Overnet, &buffer[..n]).await?;
                    }
                    Ok::<_, Error>(())
                },
            )
            .await?;
            Ok(())
        },
    )
    .await?;
    Ok(())
}

/// Retry a future until it succeeds or retries run out.
async fn retry_with_backoff<E, F>(
    mut backoff: std::time::Duration,
    max_backoff: std::time::Duration,
    f: impl Fn() -> F,
) where
    F: futures::Future<Output = Result<(), E>>,
    E: std::fmt::Debug,
{
    loop {
        match f().await {
            Ok(()) => return,
            Err(e) => {
                log::warn!("Operation failed: {:?} -- retrying in {:?}", e, backoff);
                wait_until(Instant::now() + backoff).await;
                backoff = std::cmp::min(backoff * 2, max_backoff);
            }
        }
    }
}

async fn run_overnet_inner(
    rx: Arc<Mutex<futures::channel::mpsc::UnboundedReceiver<OvernetCommand>>>,
) -> Result<(), Error> {
    let mut rx = rx.lock();
    let node_id = overnet_core::generate_node_id();
    log::trace!("Hoist node id:  {}", node_id.0);
    let node = Router::new(
        RouterOptions::new()
            .export_diagnostics(fidl_fuchsia_overnet_protocol::Implementation::HoistRustCrate)
            .set_node_id(node_id)
            .set_quic_server_key_file(hard_coded_server_key()?)
            .set_quic_server_cert_file(hard_coded_server_cert()?),
    )?;

    {
        let node = node.clone();
        spawn(retry_with_backoff(
            std::time::Duration::from_millis(100),
            std::time::Duration::from_secs(3),
            move || run_ascendd_connection(node.clone()),
        ));
    }

    // Run application loop
    loop {
        let cmd = rx.next().await;
        let desc = format!("{:?}", cmd);
        let r = match cmd {
            None => return Ok(()),
            Some(OvernetCommand::ListPeers(sender)) => {
                node.list_peers(Box::new(|peers| {
                    let _ = sender.send(peers);
                }))
                .await
            }
            Some(OvernetCommand::RegisterService(service_name, provider)) => {
                node.register_service(service_name, provider).await
            }
            Some(OvernetCommand::ConnectToService(node_id, service_name, channel)) => {
                node.connect_to_service(node_id, &service_name, channel).await
            }
            Some(OvernetCommand::AttachSocketLink(socket, options)) => {
                node.attach_socket_link(socket, options)
            }
        };
        if let Err(e) = r {
            log::warn!("cmd {} failed: {:?}", desc, e);
        }
    }
}

fn run_overnet(rx: Arc<Mutex<futures::channel::mpsc::UnboundedReceiver<OvernetCommand>>>) {
    current_thread::Runtime::new()
        .unwrap()
        .block_on(
            async move {
                if let Err(e) = run_overnet_inner(rx).await {
                    log::warn!("Main loop failed: {}", e);
                }
            }
            .unit_error()
            .boxed_local()
            .compat(),
        )
        .unwrap();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// ProxyInterface implementations

struct MeshController(Arc<Overnet>);

impl MeshControllerProxyInterface for MeshController {
    fn attach_socket_link(
        &self,
        socket: fidl::Socket,
        options: fidl_fuchsia_overnet::SocketLinkOptions,
    ) -> Result<(), fidl::Error> {
        self.0.send(OvernetCommand::AttachSocketLink(socket, options));
        Ok(())
    }
}

struct ServicePublisher(Arc<Overnet>);

impl ServicePublisherProxyInterface for ServicePublisher {
    fn publish_service(
        &self,
        service_name: &str,
        provider: ClientEnd<ServiceProviderMarker>,
    ) -> Result<(), fidl::Error> {
        self.0.send(OvernetCommand::RegisterService(service_name.to_string(), provider));
        Ok(())
    }
}

struct ServiceConsumer(Arc<Overnet>);

impl ServiceConsumerProxyInterface for ServiceConsumer {
    type ListPeersResponseFut = futures::future::MapErr<
        futures::channel::oneshot::Receiver<Vec<Peer>>,
        fn(futures::channel::oneshot::Canceled) -> fidl::Error,
    >;

    fn list_peers(&self) -> Self::ListPeersResponseFut {
        let (sender, receiver) = futures::channel::oneshot::channel();
        self.0.send(OvernetCommand::ListPeers(sender));
        receiver.map_err(|_| fidl::Error::ClientRead(zx_status::Status::PEER_CLOSED))
    }

    fn connect_to_service(
        &self,
        node: &mut fidl_fuchsia_overnet_protocol::NodeId,
        service_name: &str,
        chan: fidl::Channel,
    ) -> Result<(), fidl::Error> {
        self.0.send(OvernetCommand::ConnectToService(
            node.id.into(),
            service_name.to_string(),
            chan,
        ));
        Ok(())
    }
}

lazy_static::lazy_static! {
    static ref OVERNET: Arc<Overnet> = Arc::new(Overnet::new().unwrap());
}

pub fn connect_as_service_consumer() -> Result<impl ServiceConsumerProxyInterface, Error> {
    Ok(ServiceConsumer(OVERNET.clone()))
}

pub fn connect_as_service_publisher() -> Result<impl ServicePublisherProxyInterface, Error> {
    Ok(ServicePublisher(OVERNET.clone()))
}

pub fn connect_as_mesh_controller() -> Result<impl MeshControllerProxyInterface, Error> {
    Ok(MeshController(OVERNET.clone()))
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Executor implementation

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
