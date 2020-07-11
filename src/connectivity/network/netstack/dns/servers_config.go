// Copyright 2020 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dns

import (
	"sync"
	"time"

	"fidl/fuchsia/net/name"

	"gvisor.dev/gvisor/pkg/tcpip"
)

// DefaultDNSPort is the default port used by DNS servers.
const DefaultDNSPort = 53

// expiringDNSServerState is the state for an expiring DNS server.
type expiringDNSServerState struct {
	// Timer to invalidate a DNS server.
	//
	// May not be nil.
	timer *tcpip.CancellableTimer
}

/// Server is a DNS server with an address and configuration source.
type Server struct {
	Address tcpip.FullAddress
	Source  name.DnsServerSource
}

type OnServersConfigChangedCallback func()

// ServersConfig holds DNS resolvers' DNS servers configuration.
type ServersConfig struct {
	mu struct {
		sync.RWMutex

		// Default DNS server addresses.
		//
		// defaultServers are assumed to use DefaultDNSPort for DNS queries.
		defaultServers []tcpip.Address

		// References to slices of DNS servers configured at runtime.
		//
		// Unlike ndpServers, these servers do not have a set lifetime; they
		// are valid forever until updated.
		//
		// dhcpServers are assumed to use DefaultDNSPort for DNS queries.
		dhcpServers map[tcpip.NICID]*[]tcpip.Address

		// DNS server and associated port configured at runtime by NDP that may expire
		// after some lifetime.
		ndpServers map[tcpip.FullAddress]expiringDNSServerState

		// A cache of the available DNS server addresses and associated port that
		// may be used until invalidation.
		//
		// Must be cleared when defaultServers, dhcpServers or ndpServers
		// is updated.
		serversCache []Server

		// Function called whenever the list of servers changes.
		onServersChanged OnServersConfigChangedCallback
	}
}

func MakeServersConfig() ServersConfig {
	var d ServersConfig
	d.mu.ndpServers = make(map[tcpip.FullAddress]expiringDNSServerState)
	d.mu.dhcpServers = make(map[tcpip.NICID]*[]tcpip.Address)
	return d
}

// GetServersCache returns a list of tcpip.FullAddress to DNS servers.
//
// The expiring servers will be at the front of the list, followed by
// the runtime and default servers. The list will be deduplicated.
func (d *ServersConfig) GetServersCache() []Server {
	// If we already have a populated cache, return it.
	d.mu.RLock()
	cache := d.mu.serversCache
	d.mu.RUnlock()
	if cache != nil {
		return cache
	}

	// At this point the cache may need to be generated.

	d.mu.Lock()
	defer d.mu.Unlock()

	// We check if the cache is populated again so that the read lock is leveraged
	// above. We need to check d.mu.serversCache after taking the Write
	// lock to avoid duplicate work when multiple concurrent calls to
	// GetServersCache are made.
	//
	// Without this check, the following can occur (T1 and T2 are goroutines
	// that are trying to get the servers cache):
	//   T1: Take RLock, Drop RLock, cache == nil
	//   T2: Take RLock  Drop RLock, cache == nil
	//   T1: Take WLock
	//   T2: Attempt to take WLock, block
	//   T1: Generate cache, drop WLock
	//   T2: Obtain WLock, generate cache, drop WLock
	//
	// This example can be expanded to many more goroutines like T2.
	//
	// Here we can see that T2 unnessessarily regenerates the cache after T1. By
	// checking if the servers cache is populated after obtaining the WLock, this
	// can be avoided.
	if cache := d.mu.serversCache; cache != nil {
		return cache
	}

	have := make(map[tcpip.FullAddress]struct{})

	for s := range d.mu.ndpServers {
		// We don't check if s is already in have since d.mu.ndpServers
		// is a map - we will not see the same key twice.
		have[s] = struct{}{}
		var ndpSource name.NdpDnsServerSource
		if s.NIC != 0 {
			ndpSource.SetSourceInterface(uint64(s.NIC))
		}
		var source name.DnsServerSource
		source.SetNdp(ndpSource)
		cache = append(cache, Server{
			Address: s,
			Source:  source,
		})
	}

	// Add DHCP servers.
	for nic, serverLists := range d.mu.dhcpServers {
		for _, server := range *serverLists {
			s := tcpip.FullAddress{Addr: server, Port: DefaultDNSPort}
			if _, ok := have[s]; ok {
				// Cache already has s in it, do not duplicate it.
				continue
			}
			have[s] = struct{}{}
			var dhcpSource name.DhcpDnsServerSource
			if nic != 0 {
				dhcpSource.SetSourceInterface(uint64(nic))
			}
			var source name.DnsServerSource
			source.SetDhcp(dhcpSource)
			cache = append(cache, Server{
				Address: s,
				Source:  source,
			})
		}
	}

	// Add default servers.
	for _, server := range d.mu.defaultServers {
		s := tcpip.FullAddress{Addr: server, Port: DefaultDNSPort}
		if _, ok := have[s]; ok {
			// Cache already has s in it, do not duplicate it.
			continue
		}
		have[s] = struct{}{}
		var source name.DnsServerSource
		source.SetStaticSource(name.StaticDnsServerSource{})
		cache = append(cache, Server{
			Address: s,
			Source:  source,
		})
	}

	d.mu.serversCache = cache

	return cache
}

func (d *ServersConfig) SetOnServersChanged(callback OnServersConfigChangedCallback) {
	d.mu.Lock()
	d.mu.onServersChanged = callback
	d.mu.Unlock()
}

// SetDefaultServers sets the default list of nameservers to query.
// This usually comes from a system-wide configuration file.
// Servers are checked sequentially, in order.
// Takes ownership of the passed-in slice of addrs.
func (d *ServersConfig) SetDefaultServers(servers []tcpip.Address) {
	d.mu.Lock()
	d.mu.defaultServers = servers

	// Clear cache and notify callback
	d.mu.serversCache = nil
	cb := d.mu.onServersChanged
	d.mu.Unlock()
	if cb != nil {
		cb()
	}
}

// UpdateDhcpServers updates the list of lists of runtime servers to query (e.g.
// collected from DHCP responses) referenced by a NICID. Servers are checked
// sequentially, in order.
//
// Takes ownership of the passed-in list of serverRefs.
//
// It's possible to introduce aliasing issues if a slice pointer passed here is
// obviated later but UpdateDhcpServers isn't called again.
//
// E.g., if one of the network interface structs containing a slice pointer is
// deleted, UpdateDhcpServers should be called again with an updated list of
// runtime server refs.
func (d *ServersConfig) UpdateDhcpServers(nicid tcpip.NICID, serverRefs *[]tcpip.Address) {
	d.mu.Lock()
	if serverRefs != nil {
		d.mu.dhcpServers[nicid] = serverRefs
	} else {
		delete(d.mu.dhcpServers, nicid)
	}
	// Clear cache and notify callback
	d.mu.serversCache = nil
	cb := d.mu.onServersChanged
	d.mu.Unlock()
	if cb != nil {
		cb()
	}
}

// UpdateNdpServers updates the list of NDP-discovered servers to query.
//
// If a server already known by c, its lifetime will be refreshed. If a server
// is not known and has a non-zero lifetime, it will be stored and set to
// expire after lifetime.
//
// A lifetime value of less than 0 indicates that servers are not to expire
// (they will become valid forever until another update refreshes the lifetime).
func (d *ServersConfig) UpdateNdpServers(servers []tcpip.FullAddress, lifetime time.Duration) {
	d.mu.Lock()

	changed := false
	for _, s := range servers {
		// If s.Port is 0, then assume the default DNS port.
		if s.Port == 0 {
			s.Port = DefaultDNSPort
		}

		state, ok := d.mu.ndpServers[s]
		if lifetime != 0 {
			if !ok {
				changed = true

				// We do not yet have the server and it has a non-zero lifetime.
				s := s
				state = expiringDNSServerState{
					timer: tcpip.NewCancellableTimer(&d.mu, func() {
						// Clear the cache of DNS servers.
						d.mu.serversCache = nil
						delete(d.mu.ndpServers, s)
					}),
				}
			}

			// Refresh s's lifetime.
			state.timer.StopLocked()
			if lifetime > 0 {
				// s is valid for a finite lifetime.
				state.timer.Reset(lifetime)
			}

			d.mu.ndpServers[s] = state
		} else if ok {
			changed = true

			// We have the server and it is no longer to be used.
			state.timer.StopLocked()
			delete(d.mu.ndpServers, s)
		}
	}
	var cb OnServersConfigChangedCallback
	if changed {
		// Clear cache and set notify callback
		d.mu.serversCache = nil
		cb = d.mu.onServersChanged
	}
	d.mu.Unlock()
	if cb != nil {
		cb()
	}

}

// RemoveAllServersWithNIC removes all servers associated with the specified
// NIC.
//
// If a NIC is not specified (nicID == 0), then RemoveAllServersWithNIC does
// nothing.
func (d *ServersConfig) RemoveAllServersWithNIC(nicID tcpip.NICID) {
	if nicID == 0 {
		return
	}

	d.mu.Lock()

	for k := range d.mu.ndpServers {
		if k.NIC == nicID {
			delete(d.mu.ndpServers, k)
		}
	}

	// Clear cache and notify callback
	d.mu.serversCache = nil
	cb := d.mu.onServersChanged
	d.mu.Unlock()
	if cb != nil {
		cb()
	}
}
