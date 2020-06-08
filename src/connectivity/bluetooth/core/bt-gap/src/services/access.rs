// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    async_helpers::hanging_get::server as hanging_get,
    fidl_fuchsia_bluetooth_sys::{self as sys, AccessRequest, AccessRequestStream},
    fuchsia_async as fasync,
    fuchsia_bluetooth::types::{
        pairing_options::{BondableMode, PairingOptions},
        Peer, PeerId, Technology,
    },
    fuchsia_syslog::{fx_log_info, fx_log_warn, fx_vlog},
    futures::{Stream, StreamExt},
    parking_lot::Mutex,
    std::{collections::HashMap, mem, sync::Arc},
};

use crate::{host_dispatcher::*, watch_peers::PeerWatcher};

pub async fn run(hd: HostDispatcher, mut stream: AccessRequestStream) -> Result<(), Error> {
    fx_log_info!("fuchsia.bluetooth.sys.Access session started");
    let mut watch_peers_subscriber = hd.watch_peers().await;
    let peers_seen = Arc::new(Mutex::new(HashMap::new()));
    while let Some(event) = stream.next().await {
        handler(hd.clone(), peers_seen.clone(), &mut watch_peers_subscriber, event?).await?;
    }
    fx_log_info!("fuchsia.bluetooth.sys.Access session terminated");
    Ok(())
}

async fn handler(
    hd: HostDispatcher,
    peers_seen: Arc<Mutex<HashMap<PeerId, Peer>>>,
    watch_peers_subscriber: &mut hanging_get::Subscriber<PeerWatcher>,
    request: AccessRequest,
) -> Result<(), Error> {
    match request {
        AccessRequest::SetPairingDelegate { input, output, delegate, control_handle } => {
            match delegate.into_proxy() {
                Ok(proxy) => {
                    hd.set_io_capability(input, output);
                    hd.set_pairing_delegate(proxy);
                }
                Err(err) => {
                    fx_log_warn!(
                        "Ignoring Invalid Pairing Delegate passed to SetPairingDelegate: {}",
                        err
                    );
                    control_handle.shutdown()
                }
            }
            Ok(())
        }
        AccessRequest::SetLocalName { name, control_handle: _ } => {
            if let Err(e) = hd.set_name(name).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
        AccessRequest::SetDeviceClass { device_class, control_handle: _ } => {
            if let Err(e) = hd.set_device_class(device_class).await {
                fx_log_warn!("Error setting local name: {:?}", e);
            }
            Ok(())
        }
        AccessRequest::MakeDiscoverable { token, responder } => {
            let stream =
                token.into_stream().expect("The implementation of into_Stream() never fails");
            let mut result = hd
                .set_discoverable()
                .await
                .map(|token| {
                    watch_stream_for_session(stream, token);
                })
                .map_err(|e| e.into());
            responder.send(&mut result).map_err(Error::from)
        }
        AccessRequest::StartDiscovery { token, responder } => {
            let stream =
                token.into_stream().expect("The implementation of into_Stream() never fails");
            let mut result = hd
                .start_discovery()
                .await
                .map(|token| {
                    watch_stream_for_session(stream, token);
                })
                .map_err(|e| e.into());
            responder.send(&mut result).map_err(Error::from)
        }
        AccessRequest::WatchPeers { responder } => {
            watch_peers_subscriber.register(PeerWatcher::new(peers_seen, responder)).await.map_err(
                |e| {
                    // If we cannot register the observation, we return an error from the handler
                    // function. This terminates the stream and will drop the channel, as we are unable
                    // to fulfill our contract for WatchPeers(). The client can attempt to reconnect and
                    // if successful will receive a fresh session with initial state of the world
                    format_err!("Failed to watch peers: {:?}", e)
                },
            )
        }
        AccessRequest::Connect { id, responder } => {
            let id = PeerId::from(id);
            let result = hd.connect(id).await;
            if let Err(e) = &result {
                fx_log_warn!("Error connecting to peer {}: {:?}", id, e);
            }
            responder.send(&mut result.map_err(|e| e.into()))?;
            Ok(())
        }
        AccessRequest::Disconnect { id, responder } => {
            let id = PeerId::from(id);
            let result = hd.disconnect(id).await;
            if let Err(e) = &result {
                fx_log_warn!("Error disconnecting from peer {}: {:?}", id, e);
            }
            responder.send(&mut result.map_err(|e| e.into()))?;
            Ok(())
        }
        AccessRequest::Pair { id, options, responder } => {
            let id = PeerId::from(id);
            let opts: PairingOptions = options.into();
            // We currently do not support NonBondable mode on the classic Br/Edr transport
            // If NonBondable is asked for a Br/Edr pairing, return an InvalidArguments error
            if opts.bondable == BondableMode::NonBondable && opts.transport == Technology::Classic {
                fx_log_info!(
                    "Rejecting Pair() call; non-bondable mode is not allowed for Br/Edr transport"
                );
                responder.send(&mut Err(sys::Error::InvalidArguments))?;
                return Ok(());
            }
            let result = hd.pair(id, opts).await;
            if let Err(e) = &result {
                fx_log_warn!("Error pairing with peer {}: {:?}", id, e);
            }
            let mut result = result.map_err(|e| match e.into() {
                sys::Error::PeerNotFound => sys::Error::PeerNotFound,
                sys::Error::InvalidArguments => sys::Error::InvalidArguments,
                // We map all other host errors to Error::Failed before reporting to the caller
                _ => sys::Error::Failed,
            });
            responder.send(&mut result)?;
            Ok(())
        }
        AccessRequest::Forget { id, responder } => {
            let id = PeerId::from(id);
            let result = hd.forget(id).await;
            if let Err(e) = &result {
                fx_log_warn!("Error forgetting peer {}: {:?}", id, e);
            }
            responder.send(&mut result.map_err(|e| e.into()))?;
            Ok(())
        }
    }
}

fn watch_stream_for_session<S: Stream + Send + 'static, T: Send + 'static>(stream: S, token: T) {
    fasync::spawn(async move {
        stream.map(|_| ()).collect::<()>().await;
        // the remote end closed; drop our session token
        mem::drop(token);
        fx_vlog!(1, "ProcedureToken dropped")
    });
}
