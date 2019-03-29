// link/bridge implements a bridging LinkEndpoint
// It can be writable.
package bridge

import (
	"hash/fnv"
	"sort"
	"strings"
	"sync"

	"netstack/link"

	"github.com/google/netstack/tcpip"
	"github.com/google/netstack/tcpip/buffer"
	"github.com/google/netstack/tcpip/stack"
)

var _ stack.LinkEndpoint = (*Endpoint)(nil)
var _ stack.NetworkDispatcher = (*Endpoint)(nil)
var _ link.Controller = (*Endpoint)(nil)

type Endpoint struct {
	links           map[tcpip.LinkAddress]*BridgeableEndpoint
	dispatcher      stack.NetworkDispatcher
	mtu             uint32
	capabilities    stack.LinkEndpointCapabilities
	maxHeaderLength uint16
	linkAddress     tcpip.LinkAddress
	mu              struct {
		sync.Mutex
		onStateChange func(link.State)
	}
}

// New creates a new link from a list of BridgeableEndpoints that bridges
// packets written to it and received from any of its constituent links.
//
// The new link will have the minumum of the MTUs, the maximum of the max
// header lengths, and minimum set of the capabilities.  This function takes
// ownership of `links`.
func New(links []*BridgeableEndpoint) *Endpoint {
	sort.Slice(links, func(i, j int) bool {
		return strings.Compare(string(links[i].LinkAddress()), string(links[j].LinkAddress())) > 0
	})
	ep := &Endpoint{links: make(map[tcpip.LinkAddress]*BridgeableEndpoint)}
	h := fnv.New64()
	if len(links) > 0 {
		l := links[0]
		ep.capabilities = l.Capabilities()
		ep.mtu = l.MTU()
		ep.maxHeaderLength = l.MaxHeaderLength()
	}
	for _, l := range links {
		linkAddress := l.LinkAddress()
		ep.links[linkAddress] = l
		// Only capabilities that exist on all the links should be reported.
		ep.capabilities = CombineCapabilities(ep.capabilities, l.Capabilities())
		if mtu := l.MTU(); mtu < ep.mtu {
			ep.mtu = mtu
		}
		// maxHeaderLength is the space to reserve for possible addition
		// headers.  We want to reserve enough to suffice for all links.
		if maxHeaderLength := l.MaxHeaderLength(); maxHeaderLength > ep.maxHeaderLength {
			ep.maxHeaderLength = maxHeaderLength
		}
		if _, err := h.Write([]byte(linkAddress)); err != nil {
			panic(err)
		}
	}
	b := h.Sum(nil)[:6]
	// The second bit of the first byte indicates "locally administered".
	b[0] |= 1 << 1
	ep.linkAddress = tcpip.LinkAddress(b)
	return ep
}

// Up calls SetBridge(bridge) on all the constituent links of a bridge.
//
// This causes each constituent link to delegate dispatch to the bridge,
// meaning that received packets will be written out of or dispatched back up
// the stack for another constituent link.
func (ep *Endpoint) Up() error {
	for _, l := range ep.links {
		l.SetBridge(ep)
	}

	ep.mu.Lock()
	onStateChange := ep.mu.onStateChange
	ep.mu.Unlock()

	if onStateChange != nil {
		onStateChange(link.StateStarted)
	}

	return nil
}

// Down calls SetBridge(nil) on all the constituent links of a bridge.
//
// This causes each bridgeable endpoint to go back to its state before
// bridging, dispatching up the stack to the default NetworkDispatcher
// implementation directly.
//
// Down and Close are the same, except they call the OnStateChange callback
// with link.StateDown and link.StateClose respectively.
func (ep *Endpoint) Down() error {
	for _, l := range ep.links {
		l.SetBridge(nil)
	}

	ep.mu.Lock()
	onStateChange := ep.mu.onStateChange
	ep.mu.Unlock()

	if onStateChange != nil {
		onStateChange(link.StateDown)
	}

	return nil
}

// Close calls SetBridge(nil) on all the constituent links of a bridge.
//
// This causes each bridgeable endpoint to go back to its state before
// bridging, dispatching up the stack to the default NetworkDispatcher
// implementation directly.
//
// Down and Close are the same, except they call the OnStateChange callback
// with link.StateDown and link.StateClose respectively.
func (ep *Endpoint) Close() error {
	for _, l := range ep.links {
		l.SetBridge(nil)
	}

	ep.mu.Lock()
	onStateChange := ep.mu.onStateChange
	ep.mu.Unlock()

	if onStateChange != nil {
		onStateChange(link.StateClosed)
	}

	return nil
}

func (ep *Endpoint) SetOnStateChange(f func(link.State)) {
	ep.mu.Lock()
	defer ep.mu.Unlock()

	ep.mu.onStateChange = f
}

func (ep *Endpoint) Path() string {
	return "bridge"
}

// SetPromiscuousMode on a bridge is a no-op, since all of the constituent
// links on a bridge need to already be in promiscuous mode for bridging to
// work.
func (ep *Endpoint) SetPromiscuousMode(bool) error {
	return nil
}

// CombineCapabilities returns the capabilities restricted by the most
// restrictive of the inputs.
func CombineCapabilities(a, b stack.LinkEndpointCapabilities) stack.LinkEndpointCapabilities {
	newCapabilities := a
	// Take the minimum of CapabilityChecksumOffload and CapabilityLoopback.
	newCapabilities &= b | ^(stack.CapabilityChecksumOffload | stack.CapabilityLoopback)
	// Take the maximum of CapabilityResolutionRequired.
	newCapabilities |= b & stack.CapabilityResolutionRequired
	return newCapabilities
}

func (ep *Endpoint) MTU() uint32 {
	return ep.mtu
}

func (ep *Endpoint) Capabilities() stack.LinkEndpointCapabilities {
	return ep.capabilities
}

func (ep *Endpoint) MaxHeaderLength() uint16 {
	return ep.maxHeaderLength
}

func (ep *Endpoint) LinkAddress() tcpip.LinkAddress {
	return ep.linkAddress
}

func (ep *Endpoint) WritePacket(r *stack.Route, gso *stack.GSO, hdr buffer.Prependable, payload buffer.VectorisedView, protocol tcpip.NetworkProtocolNumber) *tcpip.Error {
	for _, l := range ep.links {
		if err := l.WritePacket(r, gso, hdr, payload, protocol); err != nil {
			return err
		}
	}
	return nil
}

func (ep *Endpoint) Attach(d stack.NetworkDispatcher) {
	ep.dispatcher = d
}

func (ep *Endpoint) IsAttached() bool {
	return ep.dispatcher != nil
}

func (ep *Endpoint) DeliverNetworkPacket(rxEP stack.LinkEndpoint, srcLinkAddr, dstLinkAddr tcpip.LinkAddress, p tcpip.NetworkProtocolNumber, vv buffer.VectorisedView) {
	broadcast := false

	switch dstLinkAddr {
	case "\xff\xff\xff\xff\xff\xff":
		broadcast = true
	case ep.linkAddress:
		ep.dispatcher.DeliverNetworkPacket(ep, srcLinkAddr, dstLinkAddr, p, vv)
		return
	default:
		if l, ok := ep.links[dstLinkAddr]; ok {
			l.Dispatcher().DeliverNetworkPacket(l, srcLinkAddr, dstLinkAddr, p, vv)
			return
		}
	}

	// The bridge `ep` isn't included in ep.links below and we don't want to write
	// out of rxEP, otherwise the rest of this function would just be
	// "ep.WritePacket and if broadcast, also deliver to ep.links."
	if broadcast {
		ep.dispatcher.DeliverNetworkPacket(ep, srcLinkAddr, dstLinkAddr, p, vv)
	}

	payload := vv
	hdr := buffer.NewPrependableFromView(payload.First())
	payload.RemoveFirst() // doesn't mutate vv

	// NB: This isn't really a valid Route; Route is a public type but cannot
	// be instantiated fully outside of the stack package, because its
	// underlying referencedNetworkEndpoint cannot be accessed.
	// This means that methods on Route that depend on accessing the
	// underlying LinkEndpoint like MTU() will panic, but it would be
	// extremely strange for the LinkEndpoint we're calling WritePacket on to
	// access itself so indirectly.
	r := stack.Route{LocalLinkAddress: srcLinkAddr, RemoteLinkAddress: dstLinkAddr, NetProto: p}

	// TODO(NET-690): Learn which destinations are on which links and restrict transmission, like a bridge.
	rxaddr := rxEP.LinkAddress()
	for linkaddr, l := range ep.links {
		if broadcast {
			l.Dispatcher().DeliverNetworkPacket(l, srcLinkAddr, dstLinkAddr, p, vv)
		}
		// Don't write back out interface from which the frame arrived
		// because that causes interoperability issues with a router.
		if linkaddr != rxaddr {
			l.WritePacket(&r, nil, hdr, payload, p)
		}
	}
}
