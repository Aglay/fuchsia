// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build !build_with_native_toolchain

package dhcp

import (
	"bytes"
	"context"
	"fmt"
	"math/rand"
	"net"
	"sync/atomic"
	"time"

	syslog "go.fuchsia.dev/fuchsia/src/lib/syslog/go"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/buffer"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/packet"
	"gvisor.dev/gvisor/pkg/waiter"
)

const (
	tag                        = "DHCP"
	defaultLeaseLength Seconds = 12 * 3600
)

type AcquiredFunc func(lost, acquired tcpip.AddressWithPrefix, cfg Config)

// Client is a DHCP client.
type Client struct {
	stack           *stack.Stack
	networkEndpoint stack.NetworkEndpoint
	xid             xid

	// info holds the Client's state as type Info.
	info atomic.Value

	acquiredFunc AcquiredFunc

	wq waiter.Queue

	// Used to ensure that only one Run goroutine per interface may be
	// permitted to run at a time. In certain cases, rapidly flapping the
	// DHCP client on and off can cause a second instance of Run to start
	// before the existing one has finished, which can violate invariants.
	// At the time of writing, TestDhcpConfiguration was creating this
	// scenario and causing panics.
	sem chan struct{}

	stats Stats

	leaseExpirationTime, renewTime, rebindTime time.Time

	// Stubbable in test.
	rand           *rand.Rand
	retransTimeout func(time.Duration) <-chan time.Time
	acquire        func(context.Context, *Client, string, *Info) (Config, error)
	now            func() time.Time
}

// Stats collects DHCP statistics per client.
type Stats struct {
	InitAcquire                 tcpip.StatCounter
	RenewAcquire                tcpip.StatCounter
	RebindAcquire               tcpip.StatCounter
	SendDiscovers               tcpip.StatCounter
	RecvOffers                  tcpip.StatCounter
	SendRequests                tcpip.StatCounter
	RecvAcks                    tcpip.StatCounter
	RecvNaks                    tcpip.StatCounter
	SendDiscoverErrors          tcpip.StatCounter
	SendRequestErrors           tcpip.StatCounter
	RecvOfferErrors             tcpip.StatCounter
	RecvOfferUnexpectedType     tcpip.StatCounter
	RecvOfferOptsDecodeErrors   tcpip.StatCounter
	RecvOfferTimeout            tcpip.StatCounter
	RecvOfferAcquisitionTimeout tcpip.StatCounter
	RecvAckErrors               tcpip.StatCounter
	RecvNakErrors               tcpip.StatCounter
	RecvAckOptsDecodeErrors     tcpip.StatCounter
	RecvAckAddrErrors           tcpip.StatCounter
	RecvAckUnexpectedType       tcpip.StatCounter
	RecvAckTimeout              tcpip.StatCounter
	RecvAckAcquisitionTimeout   tcpip.StatCounter
	ReacquireAfterNAK           tcpip.StatCounter
}

type Info struct {
	// NICID is the identifer to the associated NIC.
	NICID tcpip.NICID
	// LinkAddr is the link-address of the associated NIC.
	LinkAddr tcpip.LinkAddress
	// Acquisition is the duration within which a complete DHCP transaction must
	// complete before timing out.
	Acquisition time.Duration
	// Backoff is the duration for which the client must wait before starting a
	// new DHCP transaction after a failed transaction.
	Backoff time.Duration
	// Retransmission is the duration to wait before resending a DISCOVER or
	// REQUEST within an active transaction.
	Retransmission time.Duration
	// Acquired is the network address most recently acquired by the client.
	Acquired tcpip.AddressWithPrefix
	// Server is the network address of the DHCP server.
	Server tcpip.Address
	// State is the DHCP client state.
	State dhcpClientState
	// Assigned is the network address added by the client to its stack.
	Assigned tcpip.AddressWithPrefix
}

// NewClient creates a DHCP client.
//
// acquiredFunc will be called after each DHCP acquisition, and is responsible
// for making necessary modifications to the stack state.
func NewClient(
	s *stack.Stack,
	nicid tcpip.NICID,
	linkAddr tcpip.LinkAddress,
	acquisition,
	backoff,
	retransmission time.Duration,
	acquiredFunc AcquiredFunc,
) *Client {
	ep, err := s.GetNetworkEndpoint(nicid, header.IPv4ProtocolNumber)
	if err != nil {
		panic(fmt.Sprintf("stack.GetNetworkEndpoint(%d, header.IPv4ProtocolNumber): %s", nicid, err))
	}
	c := &Client{
		stack:           s,
		networkEndpoint: ep,
		acquiredFunc:    acquiredFunc,
		sem:             make(chan struct{}, 1),
		rand:            rand.New(rand.NewSource(time.Now().UnixNano())),
		retransTimeout:  time.After,
		acquire:         acquire,
		now:             time.Now,
	}
	c.info.Store(Info{
		NICID:          nicid,
		LinkAddr:       linkAddr,
		Acquisition:    acquisition,
		Retransmission: retransmission,
		Backoff:        backoff,
	})
	return c
}

// Info returns a copy of the synchronized state of the Info.
func (c *Client) Info() Info {
	return c.info.Load().(Info)
}

// Stats returns a reference to the Client`s stats.
func (c *Client) Stats() *Stats {
	return &c.stats
}

// Run runs the DHCP client.
//
// The function periodically searches for a new IP address.
func (c *Client) Run(ctx context.Context) {
	info := c.Info()

	nicName := c.stack.FindNICNameFromID(info.NICID)

	// For the initial iteration of the acquisition loop, the client should
	// be in the initSelecting state, corresponding to the
	// INIT->SELECTING->REQUESTING->BOUND state transition:
	// https://tools.ietf.org/html/rfc2131#section-4.4
	info.State = initSelecting

	c.sem <- struct{}{}
	defer func() { <-c.sem }()
	defer func() {
		_ = syslog.InfoTf(tag, "%s: client is stopping, cleaning up", nicName)
		c.cleanup(&info)
		// cleanup mutates info.
		c.info.Store(info)
	}()

	var timer *time.Timer

	for {
		if err := func() error {
			acquisitionTimeout := info.Acquisition

			// Adjust the timeout to make sure client is not stuck in retransmission
			// when it should transition to the next state. This can only happen for
			// two time-driven transitions: RENEW->REBIND, REBIND->INIT.
			//
			// Another time-driven transition BOUND->RENEW is not handled here because
			// the client does not have to send out any request during BOUND.
			switch s := info.State; s {
			case initSelecting:
				// Nothing to do. The client is initializing, no leases have been acquired.
				// Thus no times are set for renew, rebind, and lease expiration.
				c.stats.InitAcquire.Increment()
			case renewing:
				c.stats.RenewAcquire.Increment()
				// Instead of `time.Until`, use `now` stored on the client so
				// it can be stubbed out in test for consistency.
				if tilRebind := c.rebindTime.Sub(c.now()); tilRebind < acquisitionTimeout {
					acquisitionTimeout = tilRebind
				}
			case rebinding:
				c.stats.RebindAcquire.Increment()
				// Instead of `time.Until`, use `now` stored on the client so
				// it can be stubbed out in test for consistency.
				if tilLeaseExpire := c.leaseExpirationTime.Sub(c.now()); tilLeaseExpire < acquisitionTimeout {
					acquisitionTimeout = tilLeaseExpire
				}
			default:
				panic(fmt.Sprintf("unexpected state before acquire: %s", s))
			}

			ctx, cancel := context.WithTimeout(ctx, acquisitionTimeout)
			defer cancel()

			cfg, err := c.acquire(ctx, c, nicName, &info)
			if err != nil {
				return err
			}
			if cfg.Declined {
				c.stats.ReacquireAfterNAK.Increment()
				c.cleanup(&info)
				// Reset all the times so the client will re-acquire.
				c.leaseExpirationTime = time.Time{}
				c.renewTime = time.Time{}
				c.rebindTime = time.Time{}
				return nil
			}

			if cfg.LeaseLength == 0 {
				_ = syslog.WarnTf(tag, "%s: unspecified lease length; proceeding with default (%s)", nicName, defaultLeaseLength)
				cfg.LeaseLength = defaultLeaseLength
			}
			{
				// Based on RFC 2131 Sec. 4.4.5, this defaults to (0.5 * duration_of_lease).
				defaultRenewTime := cfg.LeaseLength / 2
				if cfg.RenewTime == 0 {
					_ = syslog.WarnTf(tag, "%s: unspecified renew time; proceeding with default (%s)", nicName, defaultRenewTime)
					cfg.RenewTime = defaultRenewTime
				}
				if cfg.RenewTime >= cfg.LeaseLength {
					_ = syslog.WarnTf(tag, "%s: renew time (%s) >= lease length (%s); proceeding with default (%s)", nicName, cfg.RenewTime, cfg.LeaseLength, defaultRenewTime)
					cfg.RenewTime = defaultRenewTime
				}
			}
			{
				// Based on RFC 2131 Sec. 4.4.5, this defaults to (0.875 * duration_of_lease).
				defaultRebindTime := cfg.LeaseLength * 875 / 1000
				if cfg.RebindTime == 0 {
					cfg.RebindTime = defaultRebindTime
				}
				if cfg.RebindTime <= cfg.RenewTime {
					_ = syslog.WarnTf(tag, "%s: rebind time (%s) <= renew time (%s); proceeding with default (%s)", nicName, cfg.RebindTime, cfg.RenewTime, defaultRebindTime)
					cfg.RebindTime = defaultRebindTime
				}
			}

			now := c.now()
			c.leaseExpirationTime = now.Add(cfg.LeaseLength.Duration())
			c.renewTime = now.Add(cfg.RenewTime.Duration())
			c.rebindTime = now.Add(cfg.RebindTime.Duration())

			c.assign(&info, info.Acquired, cfg)
			info.State = bound

			return nil
		}(); err != nil {
			if ctx.Err() != nil {
				return
			}
			_ = syslog.InfoTf(tag, "%s: %s; retrying %s", nicName, err, info.State)
		}

		// Synchronize info after attempt to acquire is complete.
		c.info.Store(info)

		// RFC 2131 Section 4.4.5
		// https://tools.ietf.org/html/rfc2131#section-4.4.5
		//
		//   T1 MUST be earlier than T2, which, in turn, MUST be earlier than
		//   the time at which the client's lease will expire.
		var next dhcpClientState
		var waitDuration time.Duration
		switch now := c.now(); {
		case !now.Before(c.leaseExpirationTime):
			next = initSelecting
		case !now.Before(c.rebindTime):
			next = rebinding
		case !now.Before(c.renewTime):
			next = renewing
		default:
			switch s := info.State; s {
			case renewing, rebinding:
				// This means the client is stuck in a bad state, because if
				// the timers are correctly set, previous cases should have matched.
				panic(fmt.Sprintf(
					"invalid client state %s, now=%s, leaseExpirationTime=%s, renewTime=%s, rebindTime=%s",
					s, now, c.leaseExpirationTime, c.renewTime, c.rebindTime,
				))
			}
			waitDuration = c.renewTime.Sub(now)
			next = renewing
		}

		// No state transition occurred, the client is retrying.
		if info.State == next {
			waitDuration = info.Backoff
		}

		if info.State != next && next != renewing {
			// Transition immediately for RENEW->REBIND, REBIND->INIT.
			if ctx.Err() != nil {
				return
			}
		} else {
			// Only (re)set timer if we actually wait on it, otherwise subsequent
			// `timer.Reset` may not work as expected because of undrained `timer.C`.
			//
			// https://golang.org/pkg/time/#Timer.Reset
			if timer == nil {
				timer = time.NewTimer(waitDuration)
			} else if waitDuration != 0 {
				timer.Reset(waitDuration)
			}
			_ = syslog.InfoTf(tag, "%s: scheduling renewal in %.fs", nicName, waitDuration.Seconds())
			select {
			case <-ctx.Done():
				return
			case <-timer.C:
			}
		}

		if info.State != initSelecting && next == initSelecting {
			_ = syslog.WarnTf(tag, "%s: lease time expired, cleaning up", nicName)
			c.cleanup(&info)
		}

		info.State = next

		// Synchronize info after any state updates.
		c.info.Store(info)
	}
}

func (c *Client) assign(info *Info, acquired tcpip.AddressWithPrefix, config Config) {
	if fn := c.acquiredFunc; fn != nil {
		fn(info.Assigned, acquired, config)
	}
	info.Assigned = acquired
}

func (c *Client) cleanup(info *Info) {
	c.assign(info, tcpip.AddressWithPrefix{}, Config{})
}

const maxBackoff = 64 * time.Second

// Exponential backoff calculates the backoff delay for this iteration (0-indexed) of retransmission.
//
// RFC 2131 section 4.1
// https://tools.ietf.org/html/rfc2131#section-4.1
//
//   The delay between retransmissions SHOULD be
//   chosen to allow sufficient time for replies from the server to be
//   delivered based on the characteristics of the internetwork between
//   the client and the server.  For example, in a 10Mb/sec Ethernet
//   internetwork, the delay before the first retransmission SHOULD be 4
//   seconds randomized by the value of a uniform random number chosen
//   from the range -1 to +1.  Clients with clocks that provide resolution
//   granularity of less than one second may choose a non-integer
//   randomization value.  The delay before the next retransmission SHOULD
//   be 8 seconds randomized by the value of a uniform number chosen from
//   the range -1 to +1.  The retransmission delay SHOULD be doubled with
//   subsequent retransmissions up to a maximum of 64 seconds.
func (c *Client) exponentialBackoff(iteration uint) time.Duration {
	jitter := time.Duration(c.rand.Int63n(int64(2*time.Second+1))) - time.Second // [-1s, +1s]
	backoff := maxBackoff
	// Guards against overflow.
	if retransmission := c.Info().Retransmission; (maxBackoff/retransmission)>>iteration != 0 {
		backoff = retransmission * (1 << iteration)
	}
	backoff += jitter
	if backoff < 0 {
		return 0
	}
	return backoff
}

func acquire(ctx context.Context, c *Client, nicName string, info *Info) (Config, error) {
	// https://tools.ietf.org/html/rfc2131#section-4.3.6 Client messages:
	//
	// ---------------------------------------------------------------------
	// |              |INIT-REBOOT  |SELECTING    |RENEWING     |REBINDING |
	// ---------------------------------------------------------------------
	// |broad/unicast |broadcast    |broadcast    |unicast      |broadcast |
	// |server-ip     |MUST NOT     |MUST         |MUST NOT     |MUST NOT  |
	// |requested-ip  |MUST         |MUST         |MUST NOT     |MUST NOT  |
	// |ciaddr        |zero         |zero         |IP address   |IP address|
	// ---------------------------------------------------------------------
	writeTo := tcpip.FullAddress{
		Addr: header.IPv4Broadcast,
		Port: ServerPort,
		NIC:  info.NICID,
	}

	ep, err := packet.NewEndpoint(c.stack, true /* cooked */, header.IPv4ProtocolNumber, &c.wq)
	if err != nil {
		return Config{}, fmt.Errorf("packet.NewEndpoint(_, true, header.IPv4ProtocolNumber, _): %s", err)
	}
	defer ep.Close()

	recvOn := tcpip.FullAddress{
		NIC: info.NICID,
	}
	if err := ep.Bind(recvOn); err != nil {
		return Config{}, fmt.Errorf("ep.Bind(%+v): %s", recvOn, err)
	}

	switch info.State {
	case initSelecting:
	case renewing:
		writeTo.Addr = info.Server
	case rebinding:
	default:
		panic(fmt.Sprintf("unknown client state: c.State=%s", info.State))
	}

	we, ch := waiter.NewChannelEntry(nil)
	c.wq.EventRegister(&we, waiter.EventIn)
	defer c.wq.EventUnregister(&we)

	if _, err := c.rand.Read(c.xid[:]); err != nil {
		return Config{}, fmt.Errorf("c.rand.Read(): %w", err)
	}

	commonOpts := options{
		{optParamReq, []byte{
			1,  // request subnet mask
			3,  // request router
			15, // domain name
			6,  // domain name server
		}},
	}
	requestedAddr := info.Acquired
	if info.State == initSelecting {
		discOpts := append(options{
			{optDHCPMsgType, []byte{byte(dhcpDISCOVER)}},
		}, commonOpts...)
		if len(requestedAddr.Address) != 0 {
			discOpts = append(discOpts, option{optReqIPAddr, []byte(requestedAddr.Address)})
		}

	retransmitDiscover:
		for i := uint(0); ; i++ {
			if err := c.send(
				ctx,
				nicName,
				info,
				discOpts,
				writeTo,
				false, /* broadcast */
				false, /* ciaddr */
			); err != nil {
				c.stats.SendDiscoverErrors.Increment()
				return Config{}, fmt.Errorf("%s: %w", dhcpDISCOVER, err)
			}
			c.stats.SendDiscovers.Increment()

			// Receive a DHCPOFFER message from a responding DHCP server.
			retransmit := c.retransTimeout(c.exponentialBackoff(i))
			for {
				result, retransmit, err := c.recv(ctx, nicName, ep, ch, retransmit)
				if err != nil {
					if retransmit {
						c.stats.RecvOfferAcquisitionTimeout.Increment()
					} else {
						c.stats.RecvOfferErrors.Increment()
					}
					return Config{}, fmt.Errorf("recv %s: %w", dhcpOFFER, err)
				}
				if retransmit {
					c.stats.RecvOfferTimeout.Increment()
					_ = syslog.WarnTf(tag, "%s: recv timeout waiting for %s; retransmitting %s", nicName, dhcpOFFER, dhcpDISCOVER)
					continue retransmitDiscover
				}

				if result.typ != dhcpOFFER {
					c.stats.RecvOfferUnexpectedType.Increment()
					_ = syslog.InfoTf(tag, "%s: got DHCP type = %s from %s, want = %s; discarding", nicName, result.typ, result.source, dhcpOFFER)
					continue
				}
				c.stats.RecvOffers.Increment()

				var cfg Config
				if err := cfg.decode(result.options); err != nil {
					c.stats.RecvOfferOptsDecodeErrors.Increment()
					return Config{}, fmt.Errorf("%s decode: %w", result.typ, err)
				}

				// We can overwrite the client's server notion, since there's no
				// atomicity required for correctness.
				//
				// We do not perform sophisticated offer selection and instead merely
				// select the first valid offer we receive.
				info.Server = cfg.ServerAddress

				if len(cfg.SubnetMask) == 0 {
					cfg.SubnetMask = tcpip.AddressMask(net.IP(result.yiaddr).DefaultMask())
				}

				prefixLen, _ := net.IPMask(cfg.SubnetMask).Size()
				requestedAddr = tcpip.AddressWithPrefix{
					Address:   result.yiaddr,
					PrefixLen: prefixLen,
				}

				_ = syslog.InfoTf(
					tag,
					"%s: got %s from %s: Address=%s, server=%s, leaseLength=%s, renewTime=%s, rebindTime=%s",
					nicName,
					result.typ,
					result.source,
					requestedAddr,
					info.Server,
					cfg.LeaseLength,
					cfg.RenewTime,
					cfg.RebindTime,
				)

				break retransmitDiscover
			}
		}
	}

	reqOpts := append(options{
		{optDHCPMsgType, []byte{byte(dhcpREQUEST)}},
	}, commonOpts...)
	if info.State == initSelecting {
		reqOpts = append(reqOpts,
			options{
				{optDHCPServer, []byte(info.Server)},
				{optReqIPAddr, []byte(requestedAddr.Address)},
			}...)
	}

retransmitRequest:
	for i := uint(0); ; i++ {
		if err := c.send(
			ctx,
			nicName,
			info,
			reqOpts,
			writeTo,
			false,                       /* broadcast */
			info.State != initSelecting, /* ciaddr */
		); err != nil {
			c.stats.SendRequestErrors.Increment()
			return Config{}, fmt.Errorf("%s: %w", dhcpREQUEST, err)
		}
		c.stats.SendRequests.Increment()

		// RFC 2131 Section 4.4.5
		// https://tools.ietf.org/html/rfc2131#section-4.4.5
		//
		//   In both RENEWING and REBINDING states, if the client receives no
		//   response to its DHCPREQUEST message, the client SHOULD wait one-half of
		//   the remaining time until T2 (in RENEWING state) and one-half of the
		//   remaining lease time (in REBINDING state), down to a minimum of 60
		//   seconds, before retransmitting the DHCPREQUEST message.
		var retransmitAfter time.Duration
		switch info.State {
		case initSelecting:
			retransmitAfter = c.exponentialBackoff(i)
		case renewing:
			retransmitAfter = c.rebindTime.Sub(c.now()) / 2
			if min := 60 * time.Second; retransmitAfter < min {
				retransmitAfter = min
			}
		case rebinding:
			retransmitAfter = c.leaseExpirationTime.Sub(c.now()) / 2
			if min := 60 * time.Second; retransmitAfter < min {
				retransmitAfter = min
			}
		default:
			panic(fmt.Sprintf("invalid client state %s", info.State))
		}

		// Receive a DHCPACK/DHCPNAK from the server.
		retransmit := c.retransTimeout(retransmitAfter)
		for {
			result, retransmit, err := c.recv(ctx, nicName, ep, ch, retransmit)
			if err != nil {
				if retransmit {
					c.stats.RecvAckAcquisitionTimeout.Increment()
				} else {
					c.stats.RecvAckErrors.Increment()
				}
				return Config{}, fmt.Errorf("recv %s: %w", dhcpACK, err)
			}
			if retransmit {
				c.stats.RecvAckTimeout.Increment()
				_ = syslog.WarnTf(tag, "%s: recv timeout waiting for %s; retransmitting %s", nicName, dhcpACK, dhcpREQUEST)
				continue retransmitRequest
			}

			switch result.typ {
			case dhcpACK:
				var cfg Config
				if err := cfg.decode(result.options); err != nil {
					c.stats.RecvAckOptsDecodeErrors.Increment()
					return Config{}, fmt.Errorf("%s decode: %w", result.typ, err)
				}
				prefixLen, _ := net.IPMask(cfg.SubnetMask).Size()
				if addr := (tcpip.AddressWithPrefix{
					Address:   result.yiaddr,
					PrefixLen: prefixLen,
				}); addr != requestedAddr {
					c.stats.RecvAckAddrErrors.Increment()
					return Config{}, fmt.Errorf("%s with unexpected address=%s expected=%s", result.typ, addr, requestedAddr)
				}
				c.stats.RecvAcks.Increment()

				// Now that we've successfully acquired the address, update the client state.
				info.Acquired = requestedAddr
				_ = syslog.InfoTf(tag, "%s: got %s from %s with leaseLength=%s", nicName, result.typ, result.source, cfg.LeaseLength)
				return cfg, nil
			case dhcpNAK:
				if msg := result.options.message(); len(msg) != 0 {
					c.stats.RecvNakErrors.Increment()
					return Config{}, fmt.Errorf("%s: %x", result.typ, msg)
				}
				c.stats.RecvNaks.Increment()
				_ = syslog.InfoTf(tag, "%s: got %s from %s", nicName, result.typ, result.source)
				// We lost the lease.
				return Config{
					Declined: true,
				}, nil
			default:
				c.stats.RecvAckUnexpectedType.Increment()
				_ = syslog.InfoTf(tag, "%s: got DHCP type = %s from %s, want = %s or %s; discarding", nicName, result.typ, result.source, dhcpACK, dhcpNAK)
				continue
			}
		}
	}
}

func (c *Client) send(
	ctx context.Context,
	nicName string,
	info *Info,
	opts options,
	writeTo tcpip.FullAddress,
	broadcast,
	ciaddr bool,
) error {
	dhcpLength := headerBaseSize + opts.len() + 1
	b := buffer.NewPrependable(header.IPv4MinimumSize + header.UDPMinimumSize + dhcpLength)
	dhcpPayload := hdr(b.Prepend(dhcpLength))
	dhcpPayload.init()
	dhcpPayload.setOp(opRequest)
	if n, l := copy(dhcpPayload.xidbytes(), c.xid[:]), len(c.xid); n != l {
		panic(fmt.Sprintf("failed to copy xid bytes, want=%d got=%d", l, n))
	}
	if broadcast {
		dhcpPayload.setBroadcast()
	}
	if ciaddr {
		ciaddr := info.Assigned.Address
		if n, l := copy(dhcpPayload.ciaddr(), ciaddr), len(ciaddr); n != l {
			panic(fmt.Sprintf("failed to copy ciaddr bytes, want=%d got=%d", l, n))
		}
	}

	chaddr := info.LinkAddr
	if n, l := copy(dhcpPayload.chaddr(), chaddr), len(chaddr); n != l {
		panic(fmt.Sprintf("failed to copy chaddr bytes, want=%d got=%d", l, n))
	}
	dhcpPayload.setOptions(opts)

	typ, err := opts.dhcpMsgType()
	if err != nil {
		panic(err)
	}

	_ = syslog.InfoTf(
		tag,
		"%s: send %s from %s:%d to %s:%d on NIC:%d (bcast=%t ciaddr=%t)",
		nicName,
		typ,
		info.Assigned.Address,
		ClientPort,
		writeTo.Addr,
		writeTo.Port,
		writeTo.NIC,
		broadcast,
		ciaddr,
	)

	// TODO(https://gvisor.dev/issues/4957): Use more streamlined serialization
	// functions when available.

	// Initialize the UDP header.
	udp := header.UDP(b.Prepend(header.UDPMinimumSize))
	length := uint16(b.UsedLength())
	udp.Encode(&header.UDPFields{
		SrcPort: ClientPort,
		DstPort: writeTo.Port,
		Length:  length,
	})
	xsum := header.PseudoHeaderChecksum(header.UDPProtocolNumber, info.Assigned.Address, writeTo.Addr, length)
	xsum = header.Checksum(dhcpPayload, xsum)
	udp.SetChecksum(^udp.CalculateChecksum(xsum))

	// Initialize the IP header.
	ip := header.IPv4(b.Prepend(header.IPv4MinimumSize))
	ip.Encode(&header.IPv4Fields{
		TotalLength: uint16(b.UsedLength()),
		Flags:       header.IPv4FlagDontFragment,
		ID:          0,
		TTL:         c.networkEndpoint.DefaultTTL(),
		TOS:         stack.DefaultTOS,
		Protocol:    uint8(header.UDPProtocolNumber),
		SrcAddr:     info.Assigned.Address,
		DstAddr:     writeTo.Addr,
	})
	ip.SetChecksum(^ip.CalculateChecksum())

	var linkAddress tcpip.LinkAddress
	{
		ch := make(chan stack.LinkResolutionResult, 1)
		err := c.stack.GetLinkAddress(info.NICID, writeTo.Addr, info.Assigned.Address, header.IPv4ProtocolNumber, func(result stack.LinkResolutionResult) {
			ch <- result
		})
		switch err.(type) {
		case nil:
			result := <-ch
			if result.Success {
				linkAddress = result.LinkAddress
			} else {
				err = &tcpip.ErrTimeout{}
			}
		case *tcpip.ErrWouldBlock:
			select {
			case <-ctx.Done():
				return fmt.Errorf("client address resolution: %w", ctx.Err())
			case result := <-ch:
				if result.Success {
					linkAddress = result.LinkAddress
					err = nil
				} else {
					err = &tcpip.ErrTimeout{}
				}
			}
		}
		if err != nil {
			return fmt.Errorf("failed to resolve link address: %s", err)
		}
	}

	if err := c.stack.WritePacketToRemote(
		writeTo.NIC,
		linkAddress,
		header.IPv4ProtocolNumber,
		b.View().ToVectorisedView(),
	); err != nil {
		return fmt.Errorf("failed to write packet: %s", err)
	}
	return nil
}

type recvResult struct {
	source  tcpip.Address
	yiaddr  tcpip.Address
	options options
	typ     dhcpMsgType
}

func (c *Client) recv(
	ctx context.Context,
	nicName string,
	ep tcpip.Endpoint,
	read <-chan struct{},
	retransmit <-chan time.Time,
) (recvResult, bool, error) {
	var b bytes.Buffer
	for {
		b.Reset()

		res, err := ep.Read(&b, tcpip.ReadOptions{
			NeedRemoteAddr:     true,
			NeedLinkPacketInfo: true,
		})
		senderAddr := tcpip.LinkAddress(res.RemoteAddr.Addr)
		if _, ok := err.(*tcpip.ErrWouldBlock); ok {
			select {
			case <-read:
				continue
			case <-retransmit:
				return recvResult{}, true, nil
			case <-ctx.Done():
				return recvResult{}, true, fmt.Errorf("read: %w", ctx.Err())
			}
		}
		if err != nil {
			return recvResult{}, false, fmt.Errorf("read: %s", err)
		}

		if res.LinkPacketInfo.Protocol != header.IPv4ProtocolNumber {
			continue
		}

		switch res.LinkPacketInfo.PktType {
		case tcpip.PacketHost, tcpip.PacketBroadcast:
		default:
			continue
		}

		v := b.Bytes()
		ip := header.IPv4(v)
		if !ip.IsValid(len(v)) {
			_ = syslog.WarnTf(
				tag,
				"%s: received malformed IP frame from %s; discarding %d bytes",
				nicName,
				senderAddr,
				len(v),
			)
			continue
		}
		// TODO(https://gvisor.dev/issues/5049): Abstract away checksum validation when possible.
		if ip.CalculateChecksum() != 0xffff {
			_ = syslog.WarnTf(
				tag,
				"%s: received damaged IP frame from %s; discarding %d bytes",
				nicName,
				senderAddr,
				len(v),
			)
			continue
		}
		if ip.More() || ip.FragmentOffset() != 0 {
			_ = syslog.WarnTf(
				tag,
				"%s: received fragmented IP frame from %s; discarding %d bytes",
				nicName,
				senderAddr,
				len(v),
			)
			continue
		}
		if ip.TransportProtocol() != header.UDPProtocolNumber {
			continue
		}
		udp := header.UDP(ip.Payload())
		if len(udp) < header.UDPMinimumSize {
			_ = syslog.WarnTf(
				tag,
				"%s: received malformed UDP frame (%s@%s -> %s); discarding %d bytes",
				nicName,
				ip.SourceAddress(),
				senderAddr,
				ip.DestinationAddress(),
				len(udp),
			)
			continue
		}
		if udp.DestinationPort() != ClientPort {
			continue
		}
		if udp.Length() > uint16(len(udp)) {
			_ = syslog.WarnTf(
				tag,
				"%s: received malformed UDP frame (%s@%s -> %s); discarding %d bytes",
				nicName,
				ip.SourceAddress(),
				senderAddr,
				ip.DestinationAddress(),
				len(udp),
			)
			continue
		}
		payload := udp.Payload()
		if xsum := udp.Checksum(); xsum != 0 {
			xsum := header.PseudoHeaderChecksum(header.UDPProtocolNumber, ip.DestinationAddress(), ip.SourceAddress(), udp.Length())
			xsum = header.Checksum(payload, xsum)
			if udp.CalculateChecksum(xsum) != 0xffff {
				_ = syslog.WarnTf(
					tag,
					"%s: received damaged UDP frame (%s@%s -> %s); discarding %d bytes",
					nicName,
					ip.SourceAddress(),
					senderAddr,
					ip.DestinationAddress(),
					len(udp),
				)
				continue
			}
		}

		h := hdr(payload)
		if !h.isValid() {
			return recvResult{}, false, fmt.Errorf("invalid hdr: %x", h)
		}

		if op := h.op(); op != opReply {
			return recvResult{}, false, fmt.Errorf("op-code=%s, want=%s", h, opReply)
		}

		if !bytes.Equal(h.xidbytes(), c.xid[:]) {
			// This message is for another client, ignore silently.
			continue
		}

		{
			opts, err := h.options()
			if err != nil {
				return recvResult{}, false, fmt.Errorf("invalid options: %w", err)
			}

			typ, err := opts.dhcpMsgType()
			if err != nil {
				return recvResult{}, false, fmt.Errorf("invalid type: %w", err)
			}

			return recvResult{
				source:  ip.SourceAddress(),
				yiaddr:  tcpip.Address(h.yiaddr()),
				options: opts,
				typ:     typ,
			}, false, nil
		}
	}
}
