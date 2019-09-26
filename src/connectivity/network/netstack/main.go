// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"flag"
	"log"
	"os"
	"reflect"
	"runtime"
	"sync"
	"syscall/zx"
	"syscall/zx/fidl"

	"app/context"
	"syslog"

	"netstack/connectivity"
	"netstack/dns"
	"netstack/filter"
	"netstack/link/eth"

	"fidl/fuchsia/device"
	"fidl/fuchsia/inspect"
	"fidl/fuchsia/net"
	"fidl/fuchsia/net/stack"
	"fidl/fuchsia/netstack"
	"fidl/fuchsia/posix/socket"

	"github.com/google/netstack/tcpip"
	"github.com/google/netstack/tcpip/network/arp"
	"github.com/google/netstack/tcpip/network/ipv4"
	"github.com/google/netstack/tcpip/network/ipv6"
	tcpipstack "github.com/google/netstack/tcpip/stack"
	"github.com/google/netstack/tcpip/transport/icmp"
	"github.com/google/netstack/tcpip/transport/tcp"
	"github.com/google/netstack/tcpip/transport/udp"
)

func Main() {
	logLevel := syslog.InfoLevel

	flags := flag.NewFlagSet(os.Args[0], flag.ContinueOnError)
	sniff := flags.Bool("sniff", false, "Enable the sniffer")
	flags.Var(&logLevel, "verbosity", "Set the logging verbosity")
	if err := flags.Parse(os.Args[1:]); err != nil {
		panic(err)
	}

	ctx := context.CreateFromStartupInfo()

	l, err := syslog.NewLogger(syslog.LogInitOptions{
		LogLevel:                      logLevel,
		MinSeverityForFileAndLineInfo: syslog.InfoLevel,
		Tags:                          []string{"netstack"},
	})
	if err != nil {
		panic(err)
	}
	syslog.SetDefaultLogger(l)
	log.SetOutput(&syslog.Writer{Logger: l})
	log.SetFlags(log.Lshortfile)

	stk := tcpipstack.New(tcpipstack.Options{
		NetworkProtocols: []tcpipstack.NetworkProtocol{
			arp.NewProtocol(),
			ipv4.NewProtocol(),
			ipv6.NewProtocol(),
		},
		TransportProtocols: []tcpipstack.TransportProtocol{
			icmp.NewProtocol4(),
			tcp.NewProtocol(),
			udp.NewProtocol(),
		},
		HandleLocal: true,
		// Raw sockets are typically used for implementing custom protocols. We intend
		// to support custom protocols through structured FIDL APIs in the future, so
		// disable raw sockets to prevent them from accidentally becoming load-bearing.
		UnassociatedFactory: nil,
	})
	if err := stk.SetTransportProtocolOption(tcp.ProtocolNumber, tcp.SACKEnabled(true)); err != nil {
		syslog.Fatalf("method SetTransportProtocolOption(%v, tcp.SACKEnabled(true)) failed: %v", tcp.ProtocolNumber, err)
	}

	arena, err := eth.NewArena()
	if err != nil {
		syslog.Fatalf("ethernet: %s", err)
	}

	req, np, err := device.NewNameProviderInterfaceRequest()
	if err != nil {
		syslog.Fatalf("could not connect to device name provider service: %s", err)
	}

	ctx.ConnectToEnvService(req)

	ns := &Netstack{
		arena:        arena,
		dnsClient:    dns.NewClient(stk),
		nameProvider: np,
		sniff:        *sniff,
	}
	ns.mu.ifStates = make(map[tcpip.NICID]*ifState)
	ns.mu.stack = stk

	if err := ns.addLoopback(); err != nil {
		syslog.Fatalf("loopback: %s", err)
	}

	var netstackService netstack.NetstackService

	ns.OnInterfacesChanged = func(interfaces2 []netstack.NetInterface2) {
		connectivity.InferAndNotify(interfaces2)
		// TODO(NET-2078): Switch to the new NetInterface struct once Chromium stops
		// using netstack.fidl.
		interfaces := interfaces2ListToInterfacesList(interfaces2)
		for _, key := range netstackService.BindingKeys() {
			if p, ok := netstackService.EventProxyFor(key); ok {
				if err := p.OnInterfacesChanged(interfaces); err != nil {
					syslog.Warnf("OnInterfacesChanged failed: %v", err)
				}
			}
		}
	}

	const counters = "counters"

	stats := reflect.ValueOf(stk.Stats())
	var inspectService inspect.InspectService
	ctx.OutgoingService.AddObjects(counters, &context.DirectoryWrapper{
		Directory: &context.DirectoryWrapper{
			Directory: &statCounterInspectImpl{
				svc:   &inspectService,
				name:  "Networking Stat Counters",
				Value: stats,
			},
		},
	})

	countersDirectory := context.DirectoryWrapper{
		Directory: &reflectNode{
			Value: stats,
		},
	}

	ctx.OutgoingService.AddDebug(counters, &countersDirectory)

	ctx.OutgoingService.AddService(
		netstack.NetstackName,
		&netstack.NetstackStub{Impl: &netstackImpl{
			ns:    ns,
			getIO: countersDirectory.GetDirectory,
		}},
		func(s fidl.Stub, c zx.Channel) error {
			k, err := netstackService.BindingSet.Add(s, c, nil)
			if err != nil {
				syslog.Fatalf("%v", err)
			}
			// Send a synthetic InterfacesChanged event to each client when they join
			// Prevents clients from having to race GetInterfaces / InterfacesChanged.
			if p, ok := netstackService.EventProxyFor(k); ok {
				ns.mu.Lock()
				interfaces2 := ns.getNetInterfaces2Locked()
				interfaces := interfaces2ListToInterfacesList(interfaces2)
				ns.mu.Unlock()

				if err := p.OnInterfacesChanged(interfaces); err != nil {
					syslog.Warnf("OnInterfacesChanged failed: %v", err)
				}
			}
			return nil
		},
	)

	var dnsService netstack.ResolverAdminService
	ctx.OutgoingService.AddService(
		netstack.ResolverAdminName,
		&netstack.ResolverAdminStub{Impl: &dnsImpl{ns: ns}},
		func(s fidl.Stub, c zx.Channel) error {
			_, err := dnsService.BindingSet.Add(s, c, nil)
			return err
		},
	)

	var stackService stack.StackService
	ctx.OutgoingService.AddService(
		stack.StackName,
		&stack.StackStub{Impl: &stackImpl{
			ns: ns,
		}},
		func(s fidl.Stub, c zx.Channel) error {
			_, err := stackService.BindingSet.Add(s, c, nil)
			return err
		},
	)

	var logService stack.LogService
	ctx.OutgoingService.AddService(
		stack.LogName,
		&stack.LogStub{Impl: &logImpl{logger: l}},
		func(s fidl.Stub, c zx.Channel) error {
			_, err := logService.BindingSet.Add(s, c, nil)
			return err
		})

	var nameLookupService net.NameLookupService
	ctx.OutgoingService.AddService(
		net.NameLookupName,
		&net.NameLookupStub{Impl: &nameLookupImpl{dnsClient: ns.dnsClient}},
		func(s fidl.Stub, c zx.Channel) error {
			_, err := nameLookupService.BindingSet.Add(s, c, nil)
			return err
		},
	)

	var posixSocketProviderService socket.ProviderService
	ctx.OutgoingService.AddService(
		socket.ProviderName,
		&socket.ProviderStub{Impl: &providerImpl{ns: ns}},
		func(s fidl.Stub, c zx.Channel) error {
			_, err := posixSocketProviderService.BindingSet.Add(s, c, nil)
			return err
		},
	)

	if err := connectivity.AddOutgoingService(ctx); err != nil {
		syslog.Fatalf("%v", err)
	}

	f := filter.New(stk.PortManager)
	if err := filter.AddOutgoingService(ctx, f); err != nil {
		syslog.Fatalf("%v", err)
	}
	ns.filter = f

	go pprofListen()

	var wg sync.WaitGroup
	for i := 0; i < runtime.NumCPU(); i++ {
		wg.Add(1)
		go func() {
			fidl.Serve()
			wg.Done()
		}()
	}
	wg.Wait()
}
