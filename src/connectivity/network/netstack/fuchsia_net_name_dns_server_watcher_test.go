// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"context"
	"sync"
	"syscall/zx/dispatch"
	"testing"
	"time"

	"netstack/dns"
	"netstack/fidlconv"

	"fidl/fuchsia/net"
	"fidl/fuchsia/net/name"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
	"gvisor.dev/gvisor/pkg/tcpip"
)

const (
	defaultServerIpv4 tcpip.Address = "\x08\x08\x08\x08"
	altServerIpv4     tcpip.Address = "\x08\x08\x04\x04"
	defaultServerIpv6 tcpip.Address = "\x20\x01\x48\x60\x48\x60\x00\x00\x00\x00\x00\x00\x00\x00\x88\x88"
)

var (
	defaultServerIPv4SocketAddress = fidlconv.ToNetSocketAddress(tcpip.FullAddress{
		NIC:  0,
		Addr: defaultServerIpv4,
		Port: dns.DefaultDNSPort,
	})
	altServerIpv4SocketAddress = fidlconv.ToNetSocketAddress(tcpip.FullAddress{
		NIC:  0,
		Addr: altServerIpv4,
		Port: dns.DefaultDNSPort,
	})
	defaultServerIpv6FullAddress = tcpip.FullAddress{
		NIC:  0,
		Addr: defaultServerIpv6,
		Port: dns.DefaultDNSPort,
	}
	defaultServerIpv6SocketAddress = fidlconv.ToNetSocketAddress(defaultServerIpv6FullAddress)

	staticDnsSource = name.DnsServerSource{
		I_dnsServerSourceTag: name.DnsServerSourceStaticSource,
	}
)

func makeDnsServer(address net.SocketAddress, source name.DnsServerSource) name.DnsServer {
	var s name.DnsServer
	s.SetAddress(address)
	s.SetSource(source)
	return s
}

func createCollection(dispatcher *dispatch.Dispatcher) *dnsServerWatcherCollection {
	watcherCollection := newDnsServerWatcherCollection(dispatcher, dns.NewClient(nil))
	watcherCollection.dnsClient.SetOnServersChanged(watcherCollection.NotifyServersChanged)
	return watcherCollection
}

func bindWatcher(t *testing.T, watcherCollection *dnsServerWatcherCollection) *name.DnsServerWatcherWithCtxInterface {
	request, watcher, err := name.NewDnsServerWatcherWithCtxInterfaceRequest()
	if err != nil {
		t.Fatalf("failed to create DnsServerWatcher channel pair: %s", err)
	}
	if err := watcherCollection.Bind(request); err != nil {
		t.Fatalf("failed to bind watcher: %s", err)
	}
	return watcher
}

func TestDnsWatcherResolvesAndBlocks(t *testing.T) {
	dispatcher, err := dispatch.NewDispatcher()
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	defer func() {
		dispatcher.Close()
		wg.Wait()
	}()

	wg.Add(1)
	go func() {
		defer wg.Done()

		dispatcher.Serve()
	}()

	watcherCollection := createCollection(dispatcher)
	watcherCollection.dnsClient.SetDefaultServers([]tcpip.Address{defaultServerIpv4, defaultServerIpv6})

	watcher := bindWatcher(t, watcherCollection)
	defer func() {
		_ = watcher.Close()
	}()

	servers, err := watcher.WatchServers(context.Background())
	if err != nil {
		t.Fatalf("failed to call WatchServers: %s", err)
	}
	if diff := cmp.Diff([]name.DnsServer{
		makeDnsServer(defaultServerIPv4SocketAddress, staticDnsSource),
		makeDnsServer(defaultServerIpv6SocketAddress, staticDnsSource),
	}, servers, cmpopts.IgnoreTypes(struct{}{})); diff != "" {
		t.Fatalf("WatchServers() mismatch (-want +got):\n%s", diff)
	}

	type watchServersResult struct {
		servers []name.DnsServer
		err     error
	}

	c := make(chan watchServersResult)
	go func() {
		servers, err := watcher.WatchServers(context.Background())
		c <- watchServersResult{
			servers: servers,
			err:     err,
		}
	}()

	select {
	case res := <-c:
		t.Fatalf("WatchServers finished too early with %+v. Should've timed out.", res)
	case <-time.After(50 * time.Millisecond):
	}

	// Clear the list of servers, now we should get something on the channel.
	watcherCollection.dnsClient.SetDefaultServers(nil)

	result := <-c

	if result.err != nil {
		t.Fatalf("WatchServers failed: %s", result.err)
	}

	if diff := cmp.Diff([]name.DnsServer{}, result.servers); diff != "" {
		t.Fatalf("WatchServers() mismatch (-want +got):\n%s", diff)
	}
}

func TestDnsWatcherCancelledContext(t *testing.T) {
	dispatcher, err := dispatch.NewDispatcher()
	if err != nil {
		t.Fatal(err)
	}
	defer dispatcher.Close()

	watcherCollection := createCollection(dispatcher)
	watcher := dnsServerWatcher{
		dnsClient:  watcherCollection.dnsClient,
		dispatcher: watcherCollection.dispatcher,
		broadcast:  &watcherCollection.broadcast,
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	if _, err := watcher.WatchServers(ctx); err == nil {
		t.Fatal("expected cancelled context to produce an error")
	}

	watcher.mu.Lock()
	if !watcher.mu.isDead {
		t.Errorf("watcher must be marked as dead on context cancellation")
	}
	if watcher.mu.isHanging {
		t.Errorf("watcher must not be marked as hanging on context cancellation")
	}
	watcher.mu.Unlock()
}

func TestDnsWatcherDisallowMultiplePending(t *testing.T) {
	t.Skip("Go bindings don't currently support simultaneous calls, test is invalid.")
	dispatcher, err := dispatch.NewDispatcher()
	if err != nil {
		t.Fatal(err)
	}
	defer dispatcher.Close()
	watcherCollection := createCollection(dispatcher)

	watcher := bindWatcher(t, watcherCollection)
	defer func() {
		_ = watcher.Close()
	}()

	var wg sync.WaitGroup
	defer wg.Wait()

	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := watcher.WatchServers(context.Background())
			if err == nil {
				t.Errorf("non-nil error watching servers, expected error")
			}
		}()
	}
}

func TestDnsWatcherMultipleWatchers(t *testing.T) {
	dispatcher, err := dispatch.NewDispatcher()
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	defer func() {
		dispatcher.Close()
		wg.Wait()
	}()

	wg.Add(1)
	go func() {
		defer wg.Done()

		dispatcher.Serve()
	}()

	watcherCollection := createCollection(dispatcher)

	watcher1 := bindWatcher(t, watcherCollection)
	defer func() {
		_ = watcher1.Close()
	}()

	watcher2 := bindWatcher(t, watcherCollection)
	defer func() {
		_ = watcher2.Close()
	}()

	func() {
		// Wait for these goroutines to do their thing before allowing the outer
		// defers to tear down the watchers.
		var wg sync.WaitGroup
		defer wg.Wait()

		for _, watcher := range []*name.DnsServerWatcherWithCtxInterface{watcher1, watcher2} {
			wg.Add(1)
			go func(watcher *name.DnsServerWatcherWithCtxInterface) {
				defer wg.Done()
				servers, err := watcher.WatchServers(context.Background())
				if err != nil {
					t.Errorf("WatchServers failed: %s", err)
					return
				}
				if diff := cmp.Diff([]name.DnsServer{
					makeDnsServer(defaultServerIPv4SocketAddress, staticDnsSource),
				}, servers, cmpopts.IgnoreTypes(struct{}{})); diff != "" {
					t.Errorf("WatchServers() mismatch (-want +got):\n%s", diff)
				}
			}(watcher)
		}

		watcherCollection.dnsClient.SetDefaultServers([]tcpip.Address{defaultServerIpv4})
	}()
}

func TestDnsWatcherServerListEquality(t *testing.T) {
	addr1 := dns.Server{
		Address: tcpip.FullAddress{
			NIC:  0,
			Addr: defaultServerIpv4,
			Port: dns.DefaultDNSPort,
		},
		Source: staticDnsSource,
	}
	addr2 := dns.Server{
		Address: tcpip.FullAddress{
			NIC:  0,
			Addr: defaultServerIpv6,
			Port: dns.DefaultDNSPort,
		},
		Source: staticDnsSource,
	}
	addr3 := dns.Server{
		Address: tcpip.FullAddress{
			NIC:  0,
			Addr: defaultServerIpv4,
			Port: 8080,
		},
		Source: staticDnsSource,
	}

	testEquality := func(equals bool, l, r []dns.Server) {
		if serverListEquals(l, r) != equals {
			t.Errorf("list comparison failed %+v == %+v should be %v, got %v", l, r, equals, !equals)
		}
	}

	testEquality(true, []dns.Server{addr1, addr2, addr3}, []dns.Server{addr1, addr2, addr3})
	testEquality(true, []dns.Server{}, nil)
	testEquality(false, []dns.Server{addr1, addr2, addr3}, []dns.Server{addr3, addr2, addr1})
	testEquality(false, []dns.Server{addr1}, []dns.Server{addr3, addr2, addr1})
	testEquality(false, []dns.Server{addr1}, []dns.Server{addr2})
}

func TestDnsWatcherDifferentAddressTypes(t *testing.T) {
	dispatcher, err := dispatch.NewDispatcher()
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	defer func() {
		dispatcher.Close()
		wg.Wait()
	}()

	wg.Add(1)
	go func() {
		defer wg.Done()

		dispatcher.Serve()
	}()

	watcherCollection := createCollection(dispatcher)
	watcher := bindWatcher(t, watcherCollection)
	defer func() {
		_ = watcher.Close()
	}()

	watcherCollection.dnsClient.SetDefaultServers([]tcpip.Address{defaultServerIpv4})
	watcherCollection.dnsClient.UpdateDhcpServers(0, &[]tcpip.Address{altServerIpv4})
	watcherCollection.dnsClient.UpdateNdpServers([]tcpip.FullAddress{defaultServerIpv6FullAddress}, -1)

	checkServers := func(expect []net.SocketAddress) {
		servers, err := watcher.WatchServers(context.Background())
		if err != nil {
			t.Fatalf("WatchServers failed: %s", err)
		}
		if len(servers) != len(expect) {
			t.Fatalf("bad WatchServers result, expected %d servers got: %+v", len(expect), servers)
		}
		for i := range expect {
			if servers[i].Address != expect[i] {
				t.Fatalf("bad server at %d.\nExpected: %+v\nGot: %+v", i, expect[i], servers[i])
			}
		}
	}

	checkServers([]net.SocketAddress{defaultServerIpv6SocketAddress, altServerIpv4SocketAddress, defaultServerIPv4SocketAddress})

	// Remove the expiring server and verify result.
	watcherCollection.dnsClient.UpdateNdpServers([]tcpip.FullAddress{defaultServerIpv6FullAddress}, 0)
	checkServers([]net.SocketAddress{altServerIpv4SocketAddress, defaultServerIPv4SocketAddress})

	// Remove DHCP server and verify result.
	watcherCollection.dnsClient.UpdateDhcpServers(0, nil)
	checkServers([]net.SocketAddress{defaultServerIPv4SocketAddress})

	// Remove the last server and verify result.
	watcherCollection.dnsClient.SetDefaultServers(nil)
	checkServers([]net.SocketAddress{})
}
