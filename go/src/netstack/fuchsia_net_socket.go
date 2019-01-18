// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"log"
	"syscall/zx"
	"syscall/zx/mxerror"

	"netstack/util"

	"fidl/fuchsia/net"

	"github.com/google/netstack/tcpip"
	"github.com/google/netstack/tcpip/network/ipv4"
	"github.com/google/netstack/tcpip/network/ipv6"
	"github.com/google/netstack/tcpip/transport/ping"
	"github.com/google/netstack/tcpip/transport/tcp"
	"github.com/google/netstack/tcpip/transport/udp"
	"github.com/google/netstack/waiter"
)

// #cgo CFLAGS: -I${SRCDIR}/../../../../zircon/system/ulib/zxs/include
// #include <lib/zxs/protocol.h>
import "C"

type socketProviderImpl struct {
	ns *Netstack
}

func sockProto(typ net.SocketType, protocol net.SocketProtocol) (tcpip.TransportProtocolNumber, error) {
	switch typ {
	case net.SocketTypeStream:
		switch protocol {
		case net.SocketProtocolIp, net.SocketProtocolTcp:
			return tcp.ProtocolNumber, nil
		default:
			return 0, mxerror.Errorf(zx.ErrNotSupported, "unsupported SOCK_STREAM protocol: %d", protocol)
		}
	case net.SocketTypeDgram:
		switch protocol {
		case net.SocketProtocolIp, net.SocketProtocolUdp:
			return udp.ProtocolNumber, nil
		case net.SocketProtocolIcmp:
			return ping.ProtocolNumber4, nil
		default:
			return 0, mxerror.Errorf(zx.ErrNotSupported, "unsupported SOCK_DGRAM protocol: %d", protocol)
		}
	}
	return 0, mxerror.Errorf(zx.ErrNotSupported, "unsupported protocol: %d/%d", typ, protocol)
}

func (sp *socketProviderImpl) OpenSocket(d net.SocketDomain, t net.SocketType, p net.SocketProtocol) (zx.Socket, int32, error) {
	var netProto tcpip.NetworkProtocolNumber
	switch d {
	case net.SocketDomainInet:
		netProto = ipv4.ProtocolNumber
	case net.SocketDomainInet6:
		netProto = ipv6.ProtocolNumber
	default:
		return zx.Socket(zx.HandleInvalid), int32(zx.ErrNotSupported), nil
	}

	transProto, err := sockProto(t, p)
	if err != nil {
		return zx.Socket(zx.HandleInvalid), int32(errStatus(err)), nil
	}

	{
		wq := new(waiter.Queue)
		ep, err := sp.ns.mu.stack.NewEndpoint(transProto, netProto, wq)
		if err != nil {
			if debug {
				log.Printf("socket: new endpoint: %v", err)
			}
			return zx.Socket(zx.HandleInvalid), int32(zx.ErrInternal), nil
		}
		{
			peerS, err := newIostate(sp.ns, netProto, transProto, wq, ep, false)
			if err != nil {
				if debug {
					log.Printf("socket: new iostate: %v", err)
				}
				return zx.Socket(zx.HandleInvalid), int32(errStatus(err)), nil
			}

			return peerS, 0, nil
		}
	}
}

func (sp *socketProviderImpl) GetAddrInfo(node *string, service *string, hints *net.AddrInfoHints) (net.AddrInfoStatus, uint32, [4]net.AddrInfo, error) {
	if hints == nil {
		hints = &net.AddrInfoHints{}
	}
	if hints.SockType == 0 {
		hints.SockType = C.SOCK_STREAM
	}
	if hints.Protocol == 0 {
		switch hints.SockType {
		case C.SOCK_STREAM:
			hints.Protocol = C.IPPROTO_TCP
		case C.SOCK_DGRAM:
			hints.Protocol = C.IPPROTO_UDP
		}
	}

	transProto, err := sockProto(net.SocketType(hints.SockType), net.SocketProtocol(hints.Protocol))
	if err != nil {
		if debug {
			log.Printf("getaddrinfo: sockProto: %v", err)
		}
		return net.AddrInfoStatusSystemError, 0, [4]net.AddrInfo{}, nil
	}

	var port uint16
	if service != nil && *service != "" {
		if port, err = serviceLookup(*service, transProto); err != nil {
			if debug {
				log.Printf("getaddrinfo: serviceLookup: %v", err)
			}
			return net.AddrInfoStatusSystemError, 0, [4]net.AddrInfo{}, nil
		}
	}

	var addrs []tcpip.Address
	switch {
	case node == nil || *node == "":
		addrs = append(addrs, "\x00\x00\x00\x00")
	case *node == "localhost" || *node == sp.ns.getNodeName():
		switch hints.Family {
		case C.AF_UNSPEC:
			addrs = append(addrs, ipv4Loopback, ipv6Loopback)
		case C.AF_INET:
			addrs = append(addrs, ipv4Loopback)
		case C.AF_INET6:
			addrs = append(addrs, ipv6Loopback)
		default:
			return net.AddrInfoStatusSystemError, 0, [4]net.AddrInfo{}, nil
		}
	default:
		if addrs, err = sp.ns.dnsClient.LookupIP(*node); err != nil {
			addrs = append(addrs, util.Parse(*node))
			if debug {
				log.Printf("getaddrinfo: addr=%v, err=%v", addrs, err)
			}
		}
	}

	if len(addrs) == 0 || len(addrs[0]) == 0 {
		return net.AddrInfoStatusNoName, 0, [4]net.AddrInfo{}, nil
	}

	// Reply up to 4 addresses.
	num := uint32(0)
	var results [4]net.AddrInfo
	for _, addr := range addrs {
		ai := net.AddrInfo{
			Flags:    0,
			SockType: hints.SockType,
			Protocol: hints.Protocol,
			Port:     port,
		}

		switch len(addr) {
		case 4:
			if hints.Family != C.AF_UNSPEC && hints.Family != C.AF_INET {
				continue
			}
			ai.Family = C.AF_INET
			ai.Addr.Len = uint32(copy(ai.Addr.Val[:], addr))
		case 16:
			if hints.Family != C.AF_UNSPEC && hints.Family != C.AF_INET6 {
				continue
			}
			ai.Family = C.AF_INET6
			ai.Addr.Len = uint32(copy(ai.Addr.Val[:], addr))
		default:
			if debug {
				log.Printf("getaddrinfo: len(addr)=%d, wrong size", len(addr))
			}
			// TODO: failing to resolve is a valid reply. fill out retval
			return net.AddrInfoStatusSystemError, 0, results, nil
		}

		results[num] = ai
		num++
		if int(num) == len(results) {
			break
		}
	}

	return net.AddrInfoStatusOk, num, results, nil
}
