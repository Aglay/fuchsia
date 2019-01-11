// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"fmt"
	"log"
	"sort"
	"strings"
	"syscall/zx"
	"syscall/zx/zxwait"

	"netstack/fidlconv"
	"netstack/link/eth"

	"fidl/fuchsia/hardware/ethernet"
	"fidl/fuchsia/net"
	"fidl/fuchsia/netstack"

	"github.com/google/netstack/tcpip"
	"github.com/google/netstack/tcpip/transport/tcp"
	"github.com/google/netstack/tcpip/transport/udp"
)

type netstackImpl struct {
	ns *Netstack
}

func toSubnets(addrs []tcpip.Address) []net.Subnet {
	out := make([]net.Subnet, len(addrs))
	for i := range addrs {
		// TODO: prefix len?
		out[i] = net.Subnet{Addr: fidlconv.ToNetIpAddress(addrs[i]), PrefixLen: 64}
	}
	return out
}

func getInterfaces(ns *Netstack) (out []netstack.NetInterface) {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	for nicid, ifs := range ns.ifStates {
		// Long-hand for: broadaddr = ifs.nic.Addr | ^ifs.nic.Netmask
		broadaddr := []byte(ifs.nic.Addr)
		if len(ifs.nic.Netmask) != len(ifs.nic.Addr) {
			log.Printf("warning: mismatched netmask and address length for nic: %+v\n", ifs.nic)
			continue
		}

		for i := range broadaddr {
			broadaddr[i] |= ^ifs.nic.Netmask[i]
		}

		var flags uint32
		if ifs.state == eth.StateStarted {
			flags |= netstack.NetInterfaceFlagUp
		}
		if ifs.dhcpState.enabled {
			flags |= netstack.NetInterfaceFlagDhcp
		}

		outif := netstack.NetInterface{
			Id:        uint32(nicid),
			Flags:     flags,
			Features:  ifs.nic.Features,
			Name:      ifs.nic.Name,
			Addr:      fidlconv.ToNetIpAddress(ifs.nic.Addr),
			Netmask:   fidlconv.ToNetIpAddress(tcpip.Address(ifs.nic.Netmask)),
			Broadaddr: fidlconv.ToNetIpAddress(tcpip.Address(broadaddr)),
			Hwaddr:    []uint8(ifs.statsEP.LinkAddress()[:]),
			Ipv6addrs: toSubnets(ifs.nic.Ipv6addrs),
		}

		out = append(out, outif)
	}

	sort.Slice(out, func(i, j int) bool {
		return out[i].Id < out[j].Id
	})

	return out
}

func (ni *netstackImpl) GetPortForService(service string, protocol netstack.Protocol) (port uint16, err error) {
	switch protocol {
	case netstack.ProtocolUdp:
		port, err = serviceLookup(service, udp.ProtocolNumber)
	case netstack.ProtocolTcp:
		port, err = serviceLookup(service, tcp.ProtocolNumber)
	default:
		port, err = serviceLookup(service, tcp.ProtocolNumber)
		if err != nil {
			port, err = serviceLookup(service, udp.ProtocolNumber)
		}
	}
	return port, err
}

func (ni *netstackImpl) GetAddress(name string, port uint16) ([]netstack.SocketAddress, netstack.NetErr, error) {
	// TODO: This should handle IP address strings, empty strings, "localhost", etc. Pull the logic from
	// fdio's getaddrinfo into here.
	addrs, err := ni.ns.dnsClient.LookupIP(name)
	if err != nil {
		return nil, netstack.NetErr{Status: netstack.StatusDnsError, Message: err.Error()}, nil
	}
	out := make([]netstack.SocketAddress, 0, len(addrs))
	for _, addr := range addrs {
		out = append(out, netstack.SocketAddress{
			Addr: fidlconv.ToNetIpAddress(addr),
			Port: port,
		})
	}
	return out, netstack.NetErr{Status: netstack.StatusOk}, nil
}

func (ni *netstackImpl) GetInterfaces() (out []netstack.NetInterface, err error) {
	return getInterfaces(ni.ns), nil
}

func (ni *netstackImpl) GetRouteTable() (out []netstack.RouteTableEntry, err error) {
	ni.ns.mu.Lock()
	defer ni.ns.mu.Unlock()
	table := ni.ns.mu.stack.GetRouteTable()
	return nsToRouteTable(table)
}

func nsToRouteTable(table []tcpip.Route) (out []netstack.RouteTableEntry, err error) {
	for _, route := range table {
		// Ensure that if any of the returned addresses are "empty",
		// they still have the appropriate length.
		l := 0
		if len(route.Destination) > 0 {
			l = len(route.Destination)
		} else if len(route.Mask) > 0 {
			l = len(route.Destination)
		}
		dest := route.Destination
		mask := route.Mask
		if len(dest) == 0 {
			dest = tcpip.Address(strings.Repeat("\x00", l))
		}
		if len(mask) == 0 {
			mask = tcpip.AddressMask(strings.Repeat("\x00", l))
		}

		var gatewayPtr *net.IpAddress
		if len(route.Gateway) != 0 {
			gateway := fidlconv.ToNetIpAddress(route.Gateway)
			gatewayPtr = &gateway
		}
		out = append(out, netstack.RouteTableEntry{
			Destination: fidlconv.ToNetIpAddress(dest),
			Netmask:     fidlconv.ToNetIpAddress(tcpip.Address(mask)),
			Gateway:     gatewayPtr,
			Nicid:       uint32(route.NIC),
		})
	}
	return out, nil
}

func routeTableToNs(rt []netstack.RouteTableEntry) []tcpip.Route {
	routes := make([]tcpip.Route, 0, len(rt))
	for _, r := range rt {
		var gateway tcpip.Address
		if r.Gateway != nil {
			gateway = fidlconv.ToTCPIPAddress(*r.Gateway)
		}
		routes = append(routes, tcpip.Route{
			Destination: fidlconv.ToTCPIPAddress(r.Destination),
			Mask:        tcpip.AddressMask(fidlconv.ToTCPIPAddress(r.Netmask)),
			Gateway:     gateway,
			NIC:         tcpip.NICID(r.Nicid),
		})
	}

	return routes
}

type routeTableTransactionImpl struct {
	ni              *netstackImpl
	routeTableCache []tcpip.Route
}

func (i *routeTableTransactionImpl) GetRouteTable() (out []netstack.RouteTableEntry, err error) {
	return nsToRouteTable(i.routeTableCache)
}

func (i *routeTableTransactionImpl) SetRouteTable(rt []netstack.RouteTableEntry) error {
	routes := routeTableToNs(rt)
	i.routeTableCache = routes
	return nil
}

func (i *routeTableTransactionImpl) Commit() (int32, error) {
	i.ni.ns.mu.Lock()
	defer i.ni.ns.mu.Unlock()
	i.ni.ns.mu.stack.SetRouteTable(i.routeTableCache)
	return int32(zx.ErrOk), nil
}

func (ni *netstackImpl) StartRouteTableTransaction(req netstack.RouteTableTransactionInterfaceRequest) (int32, error) {
	{
		ni.ns.mu.Lock()
		defer ni.ns.mu.Unlock()

		if ni.ns.mu.transactionRequest != nil {
			oldChannel := ni.ns.mu.transactionRequest.ToChannel()
			observed, _ := zxwait.Wait(*oldChannel.Handle(), 0, 0)
			// If the channel is neither readable nor writable, there is no
			// data left to be processed (not readable) and we can't return
			// any more results (not writable).  It's not enough to only
			// look at peerclosed because the peer can close the channel
			// while it still has data in its buffers.
			if observed&(zx.SignalChannelReadable|zx.SignalChannelWritable) == 0 {
				ni.ns.mu.transactionRequest = nil
			}
		}
		if ni.ns.mu.transactionRequest != nil {
			return int32(zx.ErrShouldWait), nil
		}
		ni.ns.mu.transactionRequest = &req
	}
	var routeTableService netstack.RouteTableTransactionService
	transaction := routeTableTransactionImpl{
		ni:              ni,
		routeTableCache: ni.ns.mu.stack.GetRouteTable(),
	}
	// We don't use the error handler to free the channel because it's
	// possible that the peer closes the channel before our service has
	// finished processing.
	c := req.ToChannel()
	_, err := routeTableService.Add(&transaction, c, nil)
	if err != nil {
		return int32(zx.ErrShouldWait), err
	}
	return int32(zx.ErrOk), err
}

// Add address to the given network interface.
func (ni *netstackImpl) SetInterfaceAddress(nicid uint32, address net.IpAddress, prefixLen uint8) (result netstack.NetErr, endService error) {
	log.Printf("net address %+v", address)

	nic := tcpip.NICID(nicid)
	protocol, addr, neterr := ni.ns.validateInterfaceAddress(address, prefixLen)
	if neterr.Status != netstack.StatusOk {
		return neterr, nil
	}

	if err := ni.ns.setInterfaceAddress(nic, protocol, addr, prefixLen); err != nil {
		return netstack.NetErr{Status: netstack.StatusUnknownError, Message: err.Error()}, nil
	}
	return netstack.NetErr{Status: netstack.StatusOk, Message: ""}, nil
}

func (ni *netstackImpl) RemoveInterfaceAddress(nicid uint32, address net.IpAddress, prefixLen uint8) (result netstack.NetErr, endService error) {
	nic := tcpip.NICID(nicid)
	protocol, addr, neterr := ni.ns.validateInterfaceAddress(address, prefixLen)

	if neterr.Status != netstack.StatusOk {
		return neterr, nil
	}

	if err := ni.ns.removeInterfaceAddress(nic, protocol, addr, prefixLen); err != nil {
		return netstack.NetErr{Status: netstack.StatusUnknownError, Message: err.Error()}, nil
	}

	return netstack.NetErr{Status: netstack.StatusOk, Message: ""}, nil
}

func (ni *netstackImpl) BridgeInterfaces(nicids []uint32) (netstack.NetErr, error) {
	nics := make([]tcpip.NICID, len(nicids))
	for i, n := range nicids {
		nics[i] = tcpip.NICID(n)
	}
	_, err := ni.ns.Bridge(nics)
	if err != nil {
		return netstack.NetErr{Status: netstack.StatusUnknownError}, nil
	}
	return netstack.NetErr{Status: netstack.StatusOk}, nil
}

func (ni *netstackImpl) GetAggregateStats() (stats netstack.AggregateStats, err error) {
	s := ni.ns.mu.stack.Stats()
	return netstack.AggregateStats{
		UnknownProtocolReceivedPackets: s.UnknownProtocolRcvdPackets.Value(),
		MalformedReceivedPackets:       s.MalformedRcvdPackets.Value(),
		DroppedPackets:                 s.DroppedPackets.Value(),
		IpStats: netstack.IpStats{
			PacketsReceived:          s.IP.PacketsReceived.Value(),
			InvalidAddressesReceived: s.IP.InvalidAddressesReceived.Value(),
			PacketsDelivered:         s.IP.PacketsDelivered.Value(),
			PacketsSent:              s.IP.PacketsSent.Value(),
			OutgoingPacketErrors:     s.IP.OutgoingPacketErrors.Value(),
		},
		TcpStats: netstack.TcpStats{
			ActiveConnectionOpenings:  s.TCP.ActiveConnectionOpenings.Value(),
			PassiveConnectionOpenings: s.TCP.PassiveConnectionOpenings.Value(),
			FailedConnectionAttempts:  s.TCP.FailedConnectionAttempts.Value(),
			ValidSegmentsReceived:     s.TCP.ValidSegmentsReceived.Value(),
			InvalidSegmentsReceived:   s.TCP.InvalidSegmentsReceived.Value(),
			SegmentsSent:              s.TCP.SegmentsSent.Value(),
			ResetsSent:                s.TCP.ResetsSent.Value(),
		},
		UdpStats: netstack.UdpStats{
			PacketsReceived:          s.UDP.PacketsReceived.Value(),
			UnknownPortErrors:        s.UDP.UnknownPortErrors.Value(),
			ReceiveBufferErrors:      s.UDP.ReceiveBufferErrors.Value(),
			MalformedPacketsReceived: s.UDP.MalformedPacketsReceived.Value(),
			PacketsSent:              s.UDP.PacketsSent.Value(),
		},
	}, nil
}

func (ni *netstackImpl) GetStats(nicid uint32) (stats netstack.NetInterfaceStats, err error) {
	// Pure reading of statistics. No critical section. No lock is needed.
	ifState, ok := ni.ns.ifStates[tcpip.NICID(nicid)]

	if !ok {
		// TODO(stijlist): refactor to return NetErr and use StatusUnknownInterface
		return netstack.NetInterfaceStats{}, fmt.Errorf("no such interface id: %d", nicid)
	}

	return ifState.statsEP.Stats, nil
}

func (ni *netstackImpl) SetInterfaceStatus(nicid uint32, enabled bool) error {
	if ifState, ok := ni.ns.ifStates[tcpip.NICID(nicid)]; ok {
		if enabled {
			return ifState.eth.Up()
		}
		return ifState.eth.Down()
	}

	// TODO(stijlist): refactor to return NetErr and use StatusUnknownInterface
	return fmt.Errorf("no such interface id: %d", nicid)
}

func (ni *netstackImpl) SetDhcpClientStatus(nicid uint32, enabled bool) (result netstack.NetErr, err error) {
	ifState, ok := ni.ns.ifStates[tcpip.NICID(nicid)]
	if !ok {
		return netstack.NetErr{Status: netstack.StatusUnknownInterface, Message: "unknown interface"}, nil
	}

	ifState.setDHCPStatus(enabled)
	return netstack.NetErr{Status: netstack.StatusOk, Message: ""}, nil
}

// TODO(NET-1263): Remove once clients registering with the ResolverAdmin interface
// does not crash netstack.
func (ni *netstackImpl) SetNameServers(servers []net.IpAddress) error {
	d := dnsImpl{ns: ni.ns}
	return d.SetNameServers(servers)
}

type dnsImpl struct {
	ns *Netstack
}

func (dns *dnsImpl) SetNameServers(servers []net.IpAddress) error {
	ss := make([]tcpip.Address, len(servers))

	for i, s := range servers {
		ss[i] = fidlconv.ToTCPIPAddress(s)
	}

	dns.ns.dnsClient.SetDefaultServers(ss)
	return nil
}

func (dns *dnsImpl) GetNameServers() ([]net.IpAddress, error) {
	servers := dns.ns.getDNSServers()
	out := make([]net.IpAddress, len(servers))

	for i, s := range servers {
		out[i] = fidlconv.ToNetIpAddress(s)
	}

	return out, nil
}

func (ns *netstackImpl) AddEthernetDevice(topological_path string, interfaceConfig netstack.InterfaceConfig, device ethernet.DeviceInterface) (uint32, error) {
	ifs, err := ns.ns.addEth(topological_path, interfaceConfig, &device)
	if err != nil {
		return 0, err
	}
	return uint32(ifs.nic.ID), err
}
