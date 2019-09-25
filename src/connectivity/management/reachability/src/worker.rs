// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::eventloop::Event,
    failure::Error,
    fidl_fuchsia_net_stack as stack, fidl_fuchsia_netstack as netstack, fuchsia_async as fasync,
    futures::{channel::mpsc, StreamExt, TryFutureExt},
};

pub struct EventWorker;

impl EventWorker {
    pub fn spawn(
        self,
        streams: (stack::StackEventStream, netstack::NetstackEventStream),
        event_chan: mpsc::UnboundedSender<Event>,
    ) {
        fasync::spawn_local(
            async move {
                let mut select_stream = futures::stream::select(
                    streams.0.map(|e| e.map(|x| Event::StackEvent(x))),
                    streams.1.map(|e| e.map(|x| Event::NetstackEvent(x))),
                );

                while let Some(e) = select_stream.next().await {
                    info!("Sending event: {:?}", e);
                    match e {
                        Ok(e) => event_chan.unbounded_send(e)?,
                        Err(e) => error!("Fidl event error: {}", e),
                    }
                }
                Ok(())
            }
                .unwrap_or_else(|err: Error| error!("Sending event error {:?}", err)),
        );
    }
}
