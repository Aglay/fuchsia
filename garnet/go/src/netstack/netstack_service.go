// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"fmt"
	"sort"
	"strings"
	"syscall/zx"
	"syscall/zx/zxwait"

	"syslog/logger"

	"netstack/fidlconv"
	"netstack/link"
	"netstack/routes"

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

// interfaces2ListToInterfacesList converts a NetInterface2 list into a
// NetInterface one.
func interfaces2ListToInterfacesList(ifs2 []netstack.NetInterface2) []netstack.NetInterface {
	ifs := make([]netstack.NetInterface, 0, len(ifs2))
	for _, e2 := range ifs2 {
		ifs = append(ifs, netstack.NetInterface{
			Id:        e2.Id,
			Flags:     e2.Flags,
			Features:  e2.Features,
			Name:      e2.Name,
			Addr:      e2.Addr,
			Netmask:   e2.Netmask,
			Broadaddr: e2.Broadaddr,
			Hwaddr:    e2.Hwaddr,
			Ipv6addrs: e2.Ipv6addrs,
		})
	}
	return ifs
}

func (ns *Netstack) getNetInterfaces2Locked() []netstack.NetInterface2 {
	ifStates := ns.mu.ifStates
	interfaces := make([]netstack.NetInterface2, 0, len(ifStates))
	for _, ifs := range ifStates {
		ifs.mu.Lock()
		netinterface, err := ifs.toNetInterface2Locked()
		ifs.mu.Unlock()
		if err != nil {
			logger.Warnf("failed to call ifs.toNetInterfaceLocked: %v", err)
		}
		interfaces = append(interfaces, netinterface)
	}
	return interfaces
}

func (ifs *ifState) toNetInterface2Locked() (netstack.NetInterface2, error) {
	// Long-hand for: broadaddr = ifs.mu.nic.Addr | ^ifs.mu.nic.Netmask
	broadaddr := []byte(ifs.mu.nic.Addr)
	if len(ifs.mu.nic.Netmask) != len(ifs.mu.nic.Addr) {
		return netstack.NetInterface2{}, fmt.Errorf("address length doesn't match netmask: %+v\n", ifs.mu.nic)
	}

	for i := range broadaddr {
		broadaddr[i] |= ^ifs.mu.nic.Netmask[i]
	}

	var flags uint32
	if ifs.mu.state == link.StateStarted {
		flags |= netstack.NetInterfaceFlagUp
	}
	if ifs.mu.dhcp.enabled {
		flags |= netstack.NetInterfaceFlagDhcp
	}

	return netstack.NetInterface2{
		Id:        uint32(ifs.mu.nic.ID),
		Flags:     flags,
		Features:  ifs.mu.nic.Features,
		Metric:    uint32(ifs.mu.nic.Metric),
		Name:      ifs.mu.nic.Name,
		Addr:      fidlconv.ToNetIpAddress(ifs.mu.nic.Addr),
		Netmask:   fidlconv.ToNetIpAddress(tcpip.Address(ifs.mu.nic.Netmask)),
		Broadaddr: fidlconv.ToNetIpAddress(tcpip.Address(broadaddr)),
		Hwaddr:    []uint8(ifs.statsEP.LinkAddress()[:]),
		Ipv6addrs: toSubnets(ifs.mu.nic.Ipv6addrs),
	}, nil
}

func (ns *Netstack) getInterfaces2Locked() []netstack.NetInterface2 {
	out := ns.getNetInterfaces2Locked()

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

// GetInterfaces2 returns a list of interfaces.
// TODO(NET-2078): Move this to GetInterfaces once Chromium stops using
// netstack.fidl.
func (ni *netstackImpl) GetInterfaces2() ([]netstack.NetInterface2, error) {
	ni.ns.mu.Lock()
	defer ni.ns.mu.Unlock()
	return ni.ns.getInterfaces2Locked(), nil
}

// GetInterfaces is a deprecated version that returns a list of interfaces in a
// format that Chromium supports. The new version is GetInterfaces2 which
// eventually will be renamed once Chromium is not using netstack.fidl anymore
// and this deprecated version can be removed.
func (ni *netstackImpl) GetInterfaces() ([]netstack.NetInterface, error) {
	ni.ns.mu.Lock()
	defer ni.ns.mu.Unlock()
	// Get the new interface list and convert to the old one.
	return interfaces2ListToInterfacesList(ni.ns.getInterfaces2Locked()), nil
}

// TODO(NET-2078): Move this to GetRouteTable once Chromium stops using
// netstack.fidl.
func (ni *netstackImpl) GetRouteTable2() ([]netstack.RouteTableEntry2, error) {
	return nsToRouteTable2(ni.ns.GetExtendedRouteTable()), nil
}

// GetRouteTable is a deprecated version that returns the route table in a
// format that Chromium supports. The new version is GetRouteTable2 which will
// eventually be renamed once Chromium is not using netstack.fidl anymore and
// this deprecated version can be removed.
func (ni *netstackImpl) GetRouteTable() ([]netstack.RouteTableEntry, error) {
	rt2 := nsToRouteTable2(ni.ns.GetExtendedRouteTable())
	rt := make([]netstack.RouteTableEntry, 0, len(rt2))
	for _, r2 := range rt2 {
		var gateway net.IpAddress
		if r2.Gateway != nil {
			gateway = *r2.Gateway
		} else {
			gateway = fidlconv.ToNetIpAddress(zeroIpAddr)
		}
		rt = append(rt, netstack.RouteTableEntry{
			Destination: r2.Destination,
			Netmask:     r2.Netmask,
			Gateway:     gateway,
			Nicid:       r2.Nicid,
		})
	}
	return rt, nil
}

func nsToRouteTable2(table []routes.ExtendedRoute) (out []netstack.RouteTableEntry2) {
	for _, e := range table {
		// Ensure that if any of the returned addresses are "empty",
		// they still have the appropriate length.
		l := 0
		if len(e.Route.Destination) > 0 {
			l = len(e.Route.Destination)
		} else if len(e.Route.Mask) > 0 {
			l = len(e.Route.Destination)
		}
		dest := e.Route.Destination
		mask := e.Route.Mask
		if len(dest) == 0 {
			dest = tcpip.Address(strings.Repeat("\x00", l))
		}
		if len(mask) == 0 {
			mask = tcpip.AddressMask(strings.Repeat("\x00", l))
		}

		var gatewayPtr *net.IpAddress
		if len(e.Route.Gateway) != 0 {
			gateway := fidlconv.ToNetIpAddress(e.Route.Gateway)
			gatewayPtr = &gateway
		}
		out = append(out, netstack.RouteTableEntry2{
			Destination: fidlconv.ToNetIpAddress(dest),
			Netmask:     fidlconv.ToNetIpAddress(tcpip.Address(mask)),
			Gateway:     gatewayPtr,
			Nicid:       uint32(e.Route.NIC),
			Metric:      uint32(e.Metric),
		})
	}
	return out
}

func routeToNs(r netstack.RouteTableEntry2) tcpip.Route {
	var gateway tcpip.Address
	if r.Gateway != nil {
		gateway = fidlconv.ToTCPIPAddress(*r.Gateway)
	}
	return tcpip.Route{
		Destination: fidlconv.ToTCPIPAddress(r.Destination),
		Mask:        tcpip.AddressMask(fidlconv.ToTCPIPAddress(r.Netmask)),
		Gateway:     gateway,
		NIC:         tcpip.NICID(r.Nicid),
	}
}

type routeTableTransactionImpl struct {
	ni *netstackImpl
}

func (i *routeTableTransactionImpl) AddRoute(r netstack.RouteTableEntry2) (int32, error) {
	err := i.ni.ns.AddRoute(routeToNs(r), routes.Metric(r.Metric), false /* not dynamic */)
	if err != nil {
		return int32(zx.ErrInvalidArgs), err
	}
	return int32(zx.ErrOk), nil
}

func (i *routeTableTransactionImpl) DelRoute(r netstack.RouteTableEntry2) (int32, error) {
	err := i.ni.ns.DelRoute(routeToNs(r))
	if err != nil {
		return int32(zx.ErrInvalidArgs), err
	}
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
	transaction := routeTableTransactionImpl{ni: ni}
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
func (ni *netstackImpl) SetInterfaceAddress(nicid uint32, address net.IpAddress, prefixLen uint8) (netstack.NetErr, error) {
	logger.Infof("net address %+v", address)

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

func (ni *netstackImpl) RemoveInterfaceAddress(nicid uint32, address net.IpAddress, prefixLen uint8) (netstack.NetErr, error) {
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

// SetInterfaceMetric updates the metric of an interface.
func (ni *netstackImpl) SetInterfaceMetric(nicid uint32, metric uint32) (result netstack.NetErr, err error) {
	if err := ni.ns.UpdateInterfaceMetric(tcpip.NICID(nicid), routes.Metric(metric)); err != nil {
		return netstack.NetErr{Status: netstack.StatusUnknownInterface, Message: err.Error()}, nil
	}
	return netstack.NetErr{Status: netstack.StatusOk}, nil
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
	ni.ns.mu.Lock()
	ifState, ok := ni.ns.mu.ifStates[tcpip.NICID(nicid)]
	ni.ns.mu.Unlock()

	if !ok {
		// TODO(stijlist): refactor to return NetErr and use StatusUnknownInterface
		return netstack.NetInterfaceStats{}, fmt.Errorf("no such interface id: %d", nicid)
	}

	return ifState.statsEP.Stats, nil
}

func (ni *netstackImpl) SetInterfaceStatus(nicid uint32, enabled bool) error {
	ni.ns.mu.Lock()
	ifState, ok := ni.ns.mu.ifStates[tcpip.NICID(nicid)]
	ni.ns.mu.Unlock()

	if !ok {
		// TODO(stijlist): refactor to return NetErr and use StatusUnknownInterface
		return fmt.Errorf("no such interface id: %d", nicid)
	}

	if enabled {
		return ifState.eth.Up()
	}
	return ifState.eth.Down()
}

func (ni *netstackImpl) SetDhcpClientStatus(nicid uint32, enabled bool) (netstack.NetErr, error) {
	ni.ns.mu.Lock()
	ifState, ok := ni.ns.mu.ifStates[tcpip.NICID(nicid)]
	ni.ns.mu.Unlock()

	if !ok {
		return netstack.NetErr{Status: netstack.StatusUnknownInterface, Message: "unknown interface"}, nil
	}

	ifState.mu.Lock()
	ifState.setDHCPStatusLocked(enabled)
	ifState.mu.Unlock()
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
	return uint32(ifs.mu.nic.ID), err
}
