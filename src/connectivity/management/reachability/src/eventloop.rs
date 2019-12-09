// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The special-purpose event loop used by the reachability monitor.
//!
//! This event loop receives events from netstack. Thsose events are used by the reachability
//! monitor to infer the connectivity state.

use crate::worker::EventWorker;
use failure::Error;
use futures::channel::mpsc;
use futures::prelude::*;
use reachability_core::Monitor;

/// The events that can trigger an action in the event loop.
#[derive(Debug)]
pub enum Event {
    /// An event coming from fuchsia.net.stack.
    StackEvent(fidl_fuchsia_net_stack::StackEvent),
    /// An event coming from fuchsia.netstack.
    NetstackEvent(fidl_fuchsia_netstack::NetstackEvent),
}

/// The event loop.
pub struct EventLoop {
    event_recv: mpsc::UnboundedReceiver<Event>,
    monitor: Monitor,
}

impl EventLoop {
    /// `new` returns an `EventLoop` instance.
    pub fn new(mut monitor: Monitor) -> Self {
        let (event_send, event_recv) = futures::channel::mpsc::unbounded::<Event>();

        let streams = monitor.take_event_streams();
        let event_worker = EventWorker;
        event_worker.spawn(streams, event_send.clone());
        EventLoop { event_recv, monitor }
    }

    /// `run` starts the event loop.
    pub async fn run(&mut self) -> Result<(), Error> {
        debug!("starting event loop");
        while let Some(e) = self.event_recv.next().await {
            match e {
                Event::StackEvent(event) => self.handle_stack_event(event).await,
                Event::NetstackEvent(event) => self.handle_netstack_event(event).await,
            }
        }
        Ok(())
    }

    async fn handle_stack_event(&mut self, event: fidl_fuchsia_net_stack::StackEvent) {
        debug!("stack event received {:#?}", event);
        self.monitor
            .stack_event(event)
            .await
            .unwrap_or_else(|err| error!("error updating state: {:?}", err));
    }

    async fn handle_netstack_event(&mut self, event: fidl_fuchsia_netstack::NetstackEvent) {
        debug!("netstack event received {:#?}", event);
        self.monitor
            .netstack_event(event)
            .await
            .unwrap_or_else(|err| error!("error updating state: {:?}", err));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_net as net;
    use fidl_fuchsia_netstack as netstack;
    use fuchsia_async as fasync;
    use fuchsia_async::TimeoutExt;
    use reachability_core::Pinger;

    /// log::Log implementation that uses stdout.
    ///
    /// Useful when debugging tests.
    struct Logger {}

    impl log::Log for Logger {
        fn enabled(&self, _metadata: &log::Metadata<'_>) -> bool {
            true
        }

        fn log(&self, record: &log::Record<'_>) {
            //self.printer.println(
            println!(
                "[{}] ({}) {}",
                record.level(),
                record.module_path().unwrap_or(""),
                record.args(),
            )
        }

        fn flush(&self) {}
    }

    fn net_interface(port: u32, addr: [u8; 4]) -> netstack::NetInterface {
        netstack::NetInterface {
            id: port,
            flags: netstack::NET_INTERFACE_FLAG_UP | netstack::NET_INTERFACE_FLAG_DHCP,
            features: 0,
            configuration: 0,
            name: port.to_string(),
            addr: net::IpAddress::Ipv4(net::Ipv4Address { addr }),
            netmask: net::IpAddress::Ipv4(net::Ipv4Address { addr: [255, 255, 255, 0] }),
            broadaddr: net::IpAddress::Ipv4(net::Ipv4Address { addr: [1, 2, 3, 255] }),
            ipv6addrs: vec![],
            hwaddr: [1, 2, 3, 4, 5, port as u8].to_vec(),
        }
    }

    struct Ping<'a> {
        gateway_url: &'a str,
        gateway_response: bool,
        internet_url: &'a str,
        internet_response: bool,
        default_response: bool,
    }

    impl Pinger for Ping<'_> {
        fn ping(&self, url: &str) -> bool {
            if self.gateway_url == url {
                return self.gateway_response;
            }
            if self.internet_url == url {
                return self.internet_response;
            }
            self.default_response
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_events_are_received() {
        let (event_send, event_recv) = futures::channel::mpsc::unbounded::<Event>();

        let mut monitor = Monitor::new(Box::new(Ping {
            gateway_url: "1.2.3.1",
            gateway_response: true,
            internet_url: "8.8.8.8",
            internet_response: false,
            default_response: false,
        }))
        .unwrap();
        let streams = monitor.take_event_streams();
        let event_worker = EventWorker;
        event_worker.spawn(streams, event_send.clone());
        let mut event_loop = EventLoop { event_recv, monitor };

        fasync::spawn_local(async {
            // Send event to it
            let e = Event::NetstackEvent(netstack::NetstackEvent::OnInterfacesChanged {
                interfaces: vec![net_interface(5, [1, 2, 3, 1])],
            });
            event_send.unbounded_send(e).unwrap();

            let e = Event::NetstackEvent(netstack::NetstackEvent::OnInterfacesChanged {
                interfaces: vec![net_interface(5, [1, 2, 3, 2])],
            });
            event_send.unbounded_send(e).unwrap();
            let e = Event::NetstackEvent(netstack::NetstackEvent::OnInterfacesChanged {
                interfaces: vec![net_interface(5, [1, 2, 3, 3])],
            });
            event_send.unbounded_send(e).unwrap();
            let e = Event::NetstackEvent(netstack::NetstackEvent::OnInterfacesChanged {
                interfaces: vec![net_interface(5, [1, 2, 3, 4])],
            });
            event_send.unbounded_send(e).unwrap();
            drop(event_send);
        });

        let x = event_loop
            .run()
            .on_timeout(fasync::Time::after(fuchsia_zircon::Duration::from_seconds(10)), || {
                panic!("timed out")
            })
            .await;
        println!("eventloop result {:?}", x);
        assert_eq!(event_loop.monitor.stats().events, 4);
    }
}
