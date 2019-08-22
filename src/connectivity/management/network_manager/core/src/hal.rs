// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A simple port manager.

use crate::error;
use crate::lifmgr::{self, LIFProperties, LifIpAddr};
use failure::{Error, ResultExt};
use fidl_fuchsia_net;
use fidl_fuchsia_net_stack::{InterfaceInfo, StackMarker, StackProxy};
use fidl_fuchsia_netstack::{NetstackMarker, NetstackProxy};
use fuchsia_component::client::connect_to_service;
use std::collections::HashSet;
use std::convert::TryFrom;
use std::net::IpAddr;

// StackPortId is the port ID's used by the netstack.
// This is what is passed in the stack FIDL to denote the port or nic id.
#[derive(Eq, PartialEq, Hash, Debug, Copy, Clone)]
pub struct StackPortId(u64);
impl From<u64> for StackPortId {
    fn from(p: u64) -> StackPortId {
        StackPortId(p)
    }
}
impl From<PortId> for StackPortId {
    fn from(p: PortId) -> StackPortId {
        // TODO(dpradilla): This should be based on the mapping between physical location and
        // logical (from management plane point of view) port id.
        StackPortId::from(p.to_u64())
    }
}
impl StackPortId {
    // to_u64 converts it to u64, as some FIDL interfaces use the ID as a u64.
    pub fn to_u64(self) -> u64 {
        self.0
    }
    // to_u32 converts it to u32, as some FIDL interfaces use the ID as a u32.
    pub fn to_u32(self) -> u32 {
        match u32::try_from(self.0) {
            Ok(v) => v,
            e => {
                warn!("overflow converting StackPortId {:?}: {:?}", self.0, e);
                self.0 as u32
            }
        }
    }
}

#[derive(Eq, PartialEq, Hash, Debug, Copy, Clone)]
pub struct PortId(u64);
impl From<u64> for PortId {
    fn from(p: u64) -> PortId {
        PortId(p)
    }
}
impl From<StackPortId> for PortId {
    // TODO(dpradilla): This should be based on the mapping between physical location and
    // logical (from management plane point of view) port id.
    fn from(p: StackPortId) -> PortId {
        PortId(p.to_u64())
    }
}
impl PortId {
    // to_u64 converts it to u64, as some FIDL interfaces use the ID as a u64.
    pub fn to_u64(self) -> u64 {
        self.0
    }

    // to_u32 converts it to u32, as some FIDL interfaces use the ID as a u32.
    pub fn to_u32(self) -> u32 {
        match u32::try_from(self.0) {
            Ok(v) => v,
            e => {
                warn!("overflow converting StackPortId {:?}: {:?}", self.0, e);
                self.0 as u32
            }
        }
    }
}

pub struct NetCfg {
    stack: StackProxy,
    netstack: NetstackProxy,
    // id_in_use contains interface id's currently in use by the system
    id_in_use: HashSet<StackPortId>,
}

#[derive(Debug)]
pub struct Port {
    pub id: PortId,
    pub path: String,
}

#[derive(Debug, Eq, PartialEq)]
pub struct Interface {
    pub id: PortId,
    pub name: String,
    pub addr: Option<LifIpAddr>,
    pub enabled: bool,
}

impl From<&InterfaceInfo> for Interface {
    fn from(iface: &InterfaceInfo) -> Self {
        Interface {
            id: iface.id.into(),
            name: iface.properties.topopath.clone(),
            addr:
                iface
                    .properties
                    .addresses
                    .iter()
                    .find(|&addr| match addr {
                        // Only return interfaces with an IPv4 address
                        // TODO(dpradilla) support IPv6 and interfaces with multiple IPs? (is there
                        // a use case given this context?)
                        fidl_fuchsia_net_stack::InterfaceAddress {
                            ip_address:
                                fidl_fuchsia_net::IpAddress::Ipv4(fidl_fuchsia_net::Ipv4Address {
                                    addr,
                                }),
                            ..
                        } => {
                            let ip_address = IpAddr::from(*addr);
                            !ip_address.is_loopback()
                                && !ip_address.is_unspecified()
                                && !ip_address.is_multicast()
                        }
                        _ => false,
                    })
                    .map(|addr| addr.into()),
            enabled: match iface.properties.administrative_status {
                fidl_fuchsia_net_stack::AdministrativeStatus::Enabled => true,
                fidl_fuchsia_net_stack::AdministrativeStatus::Disabled => false,
            },
        }
    }
}

impl Into<LIFProperties> for Interface {
    fn into(self) -> LIFProperties {
        LIFProperties { dhcp: false, address: self.addr, enabled: self.enabled }
    }
}

impl NetCfg {
    pub fn new() -> Result<Self, Error> {
        let stack = connect_to_service::<StackMarker>()
            .context("network_manager failed to connect to netstack")?;
        let netstack = connect_to_service::<NetstackMarker>()
            .context("network_manager failed to connect to netstack")?;
        Ok(NetCfg { stack, netstack, id_in_use: HashSet::new() })
    }

    pub fn take_event_stream(&mut self) -> fidl_fuchsia_net_stack::StackEventStream {
        self.stack.take_event_stream()
    }

    pub async fn get_interface(&mut self, port: u64) -> Option<Interface> {
        match self.stack.get_interface_info(port).await {
            Ok((Some(info), _)) => Some((&(*info)).into()),
            _ => None,
        }
    }

    // ports gets all physical ports in the system.
    pub async fn ports(&self) -> error::Result<Vec<Port>> {
        let ports = self.stack.list_interfaces().await.map_err(|_| error::Hal::OperationFailed)?;
        let p = ports
            .into_iter()
            .filter(|x| x.properties.topopath != "")
            .map(|x| Port { id: StackPortId::from(x.id).into(), path: x.properties.topopath })
            .collect::<Vec<Port>>();
        Ok(p)
    }

    /// `interfaces` returns all L3 interfaces with valid, non-local IPs in the system.
    pub async fn interfaces(&mut self) -> error::Result<Vec<Interface>> {
        let ifs = self
            .stack
            .list_interfaces()
            .await
            .map_err(|_| error::Hal::OperationFailed)?
            .iter()
            .map(|i| i.into())
            .filter(|i: &Interface| i.addr.is_some())
            .collect();
        Ok(ifs)
    }
    pub async fn create_bridge(&mut self, ports: Vec<PortId>) -> error::Result<Interface> {
        let _br = self
            .netstack
            .bridge_interfaces(&mut ports.into_iter().map(|id| StackPortId::from(id).to_u32()))
            .await;
        // Find out what was the interface created, as there is no indication from above API.
        let ifs = self.stack.list_interfaces().await.map_err(|_| error::Hal::OperationFailed)?;
        if let Some(i) = ifs.iter().find(|x| self.id_in_use.insert(StackPortId::from(x.id))) {
            Ok(i.into())
        } else {
            Err(error::NetworkManager::HAL(error::Hal::BridgeNotFound))
        }
    }

    /// `delete_bridge` deletes a bridge.
    pub async fn delete_bridge(&mut self, id: PortId) -> error::Result<()> {
        // TODO(dpradilla): what is the API for deleting a bridge? Call it
        info!("delete_bridge {:?} - Noop for now", id);
        Ok(())
    }

    /// `set_ip_address` configures an IP address.
    pub async fn set_ip_address<'a>(
        &'a mut self,
        pid: PortId,
        addr: &'a LifIpAddr,
    ) -> error::Result<()> {
        let r = self
            .stack
            .add_interface_address(
                StackPortId::from(pid).to_u64(),
                &mut addr.to_fidl_interface_address(),
            )
            .await;
        match r {
            Err(_) => Err(error::NetworkManager::HAL(error::Hal::OperationFailed)),
            Ok(r) => match r {
                None => Ok(()),
                Some(e) => {
                    println!("could not set interface address: ${:?}", e);
                    Err(error::NetworkManager::HAL(error::Hal::OperationFailed))
                }
            },
        }
    }

    /// `unset_ip_address` removes an IP address from the interface configuration.
    pub async fn unset_ip_address<'a>(
        &'a mut self,
        pid: PortId,
        addr: &'a LifIpAddr,
    ) -> error::Result<()> {
        let a = addr.to_fidl_address_and_prefix();
        // TODO(dpradilla): this needs to be changed to use the stack fidl once
        // this functionality is moved there. the u32 conversion won't be needed.
        let r = self
            .netstack
            .remove_interface_address(
                pid.to_u32(),
                &mut a.address.unwrap(),
                a.prefix_length.unwrap(),
            )
            .await;
        match r {
            Ok(fidl_fuchsia_netstack::NetErr {
                status: fidl_fuchsia_netstack::Status::Ok,
                message: _,
            }) => Ok(()),
            _ => Err(error::NetworkManager::HAL(error::Hal::OperationFailed)),
        }
    }

    pub async fn set_interface_state(&mut self, pid: PortId, state: bool) -> error::Result<()> {
        let r = if state {
            self.stack.enable_interface(StackPortId::from(pid).to_u64())
        } else {
            self.stack.disable_interface(StackPortId::from(pid).to_u64())
        };
        match r.await {
            Ok(_) => Ok(()),
            _ => Err(error::NetworkManager::HAL(error::Hal::OperationFailed)),
        }
    }

    pub async fn set_dhcp_client_state(&mut self, pid: PortId, enable: bool) -> error::Result<()> {
        let r = self.netstack.set_dhcp_client_status(StackPortId::from(pid).to_u32(), enable).await;
        match r {
            Ok(fidl_fuchsia_netstack::NetErr {
                status: fidl_fuchsia_netstack::Status::Ok,
                message: _,
            }) => Ok(()),
            _ => Err(error::NetworkManager::HAL(error::Hal::OperationFailed)),
        }
    }

    // apply_manual_address updates the configured IP address.
    async fn apply_manual_ip<'a>(
        &'a mut self,
        pid: PortId,
        current: &'a Option<LifIpAddr>,
        desired: &'a Option<LifIpAddr>,
    ) -> error::Result<()> {
        match (current, desired) {
            (Some(current_ip), Some(desired_ip)) => {
                if current_ip != current_ip {
                    // There has been a change.
                    // Remove the old one and add the new one.
                    self.unset_ip_address(pid, &current_ip).await?;
                    self.set_ip_address(pid, &desired_ip).await?;
                }
            }
            (None, Some(desired_ip)) => {
                self.set_ip_address(pid, &desired_ip).await?;
            }
            (Some(current_ip), None) => {
                self.unset_ip_address(pid, &current_ip).await?;
            }
            // Nothing to do.
            (None, None) => {}
        };
        Ok(())
    }

    /// `apply_properties` applies the indicated LIF properties.
    pub async fn apply_properties<'a>(
        &'a mut self,
        pid: PortId,
        old: &'a lifmgr::LIFProperties,
        properties: &'a lifmgr::LIFProperties,
    ) -> error::Result<()> {
        match (old.dhcp, properties.dhcp) {
            // dhcp configuration transitions from enabled to disabled.
            (true, false) => {
                // Disable dhcp and apply manual address configuration.
                self.set_dhcp_client_state(pid, properties.dhcp).await?;
                self.apply_manual_ip(pid, &old.address, &properties.address).await?;
            }
            // dhcp configuration transitions from disabled to enabled.
            (false, true) => {
                // Remove any manual IP address and enable dhcp client.
                self.apply_manual_ip(pid, &old.address, &None).await?;
                self.set_dhcp_client_state(pid, properties.dhcp).await?;
            }
            // dhcp is still disabled, check for manual IP address changes.
            (false, false) => {
                self.apply_manual_ip(pid, &old.address, &properties.address).await?;
            }
            // No changes to dhcp configuration, it is still enabled, nothing to do.
            (true, true) => {}
        }
        if old.enabled != properties.enabled {
            info!("id {:?} updating state {:?}", pid, properties.enabled);
            self.set_interface_state(pid, properties.enabled).await?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_net as net;
    use fidl_fuchsia_net_stack as stack;

    fn interface_info_with_addrs(addrs: Vec<stack::InterfaceAddress>) -> InterfaceInfo {
        InterfaceInfo {
            id: 42,
            properties: stack::InterfaceProperties {
                topopath: "test/interface/info".to_string(),
                addresses: addrs,
                administrative_status: stack::AdministrativeStatus::Enabled,

                // Unused fields
                name: "ethtest".to_string(),
                filepath: "/some/file".to_string(),
                mac: None,
                mtu: 0,
                features: 0,
                physical_status: stack::PhysicalStatus::Down,
            },
        }
    }

    fn interface_with_addr(addr: Option<LifIpAddr>) -> Interface {
        Interface {
            id: 42.into(),
            name: "test/interface/info".to_string(),
            addr: addr,
            enabled: true,
        }
    }

    fn sample_addresses() -> Vec<stack::InterfaceAddress> {
        vec![
            // Unspecified addresses are skipped.
            stack::InterfaceAddress {
                ip_address: net::IpAddress::Ipv4(net::Ipv4Address { addr: [0, 0, 0, 0] }),
                prefix_len: 24,
            },
            // Multicast addresses are skipped.
            stack::InterfaceAddress {
                ip_address: net::IpAddress::Ipv4(net::Ipv4Address { addr: [224, 0, 0, 5] }),
                prefix_len: 24,
            },
            // Loopback addresses are skipped.
            stack::InterfaceAddress {
                ip_address: net::IpAddress::Ipv4(net::Ipv4Address { addr: [127, 0, 0, 1] }),
                prefix_len: 24,
            },
            // IPv6 addresses are skipped.
            stack::InterfaceAddress {
                ip_address: net::IpAddress::Ipv6(net::Ipv6Address {
                    addr: [16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1],
                }),
                prefix_len: 8,
            },
            // First valid address, should be picked.
            stack::InterfaceAddress {
                ip_address: net::IpAddress::Ipv4(net::Ipv4Address { addr: [4, 3, 2, 1] }),
                prefix_len: 24,
            },
            // A valid address is already available, so this address should be skipped.
            stack::InterfaceAddress {
                ip_address: net::IpAddress::Ipv4(net::Ipv4Address { addr: [1, 2, 3, 4] }),
                prefix_len: 24,
            },
        ]
    }

    #[test]
    fn test_net_interface_info_into_hal_interface() {
        let info = interface_info_with_addrs(sample_addresses());
        let iface: Interface = (&info).into();
        assert_eq!(iface.name, "test/interface/info");
        assert_eq!(iface.enabled, true);
        assert_eq!(iface.addr, Some(LifIpAddr { address: IpAddr::from([4, 3, 2, 1]), prefix: 24 }));
        assert_eq!(iface.id.to_u64(), 42);
    }

    async fn handle_list_interfaces(request: stack::StackRequest) {
        match request {
            stack::StackRequest::ListInterfaces { responder } => {
                responder
                    .send(
                        &mut sample_addresses()
                            .into_iter()
                            .map(|addr| interface_info_with_addrs(vec![addr]))
                            .collect::<Vec<InterfaceInfo>>()
                            .iter_mut(),
                    )
                    .unwrap();
            }
            _ => {
                panic!("unexpected stack request: {:?}", request);
            }
        }
    }

    async fn handle_with_panic<Request: std::fmt::Debug>(request: Request) {
        panic!("unexpected request: {:?}", request);
    }

    #[fuchsia_async::run_until_stalled(test)]
    async fn test_ignore_interface_without_ip() {
        let stack: StackProxy =
            fidl::endpoints::spawn_stream_handler(handle_list_interfaces).unwrap();
        let netstack: NetstackProxy =
            fidl::endpoints::spawn_stream_handler(handle_with_panic).unwrap();
        let mut netcfg = NetCfg { stack, netstack, id_in_use: HashSet::new() };
        assert_eq!(
            netcfg.interfaces().await.unwrap(),
            // Should return only interfaces with a valid address.
            vec![
                interface_with_addr(Some(LifIpAddr {
                    address: IpAddr::from([4, 3, 2, 1]),
                    prefix: 24
                })),
                interface_with_addr(Some(LifIpAddr {
                    address: IpAddr::from([1, 2, 3, 4]),
                    prefix: 24
                })),
            ]
        );
    }
}
