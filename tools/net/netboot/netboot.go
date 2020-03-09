// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This package implements the Zircon netboot protocol.
//
// TODO(fxb/35957): Add testing for this package.
package netboot

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"strings"
	"time"

	"golang.org/x/net/ipv6"
)

// NodenameWildcard is the wildcard for discovering all nodes.
const NodenameWildcard = "*"

// Magic constants used by the netboot protocol.
const (
	baseCookie = uint32(0x12345678)
	magic      = 0xAA774217 // see //zircon/system/public/zircon/boot/netboot.h
)

// Port numbers used by the netboot protocol.
const (
	serverPort      = 33330 // netboot server port
	advertPort      = 33331 // advertisement port
	clientPortStart = 33332 // client port range start.
	clientPortEnd   = 33339 // client port range end.
)

// Commands supported by the netboot protocol.
const (
	cmdAck      = uint32(0)  // ack
	cmdCommand  = uint32(1)  // command
	cmdSendFile = uint32(2)  // send file
	cmdData     = uint32(3)  // data
	cmdBoot     = uint32(4)  // boot command
	cmdQuery    = uint32(5)  // query command
	cmdShell    = uint32(6)  // shell command
	cmdOpen     = uint32(7)  // open file
	cmdRead     = uint32(8)  // read data
	cmdWrite    = uint32(9)  // write data
	cmdClose    = uint32(10) // close file
	cmdLastData = uint32(11) //
	cmdReboot   = uint32(12) // reboot command
)

// Client implements the netboot protocol.
type Client struct {
	ServerPort int
	AdvertPort int
	Cookie     uint32
	Timeout    time.Duration
	Wait       bool
}

// netbootHeader is the netboot protocol message header.
type netbootHeader struct {
	Magic  uint32
	Cookie uint32
	Cmd    uint32
	Arg    uint32
}

// netbootMessage is the netboot protocol message.
type netbootMessage struct {
	Header netbootHeader
	Data   [1024]byte
}

// Target defines a netboot protocol target, which includes information about how
// to find said target on the network: the target's nodename, its address, the
// address from the target back to the host, and the interface used to connect
// from the host to the target.
type Target struct {
	// Nodename is target's nodename: thumb-set-human-neon is an example.
	// This is derived from the NIC mac address.
	Nodename string

	// TargetAddress is the address of the target from the host.
	TargetAddress net.IP

	// HostAddress is the "local" address, i.e. the one to which the target
	// is responding. This would be the address the target would send to in
	// order to communicate with the host.
	HostAddress net.IP

	// Interface is the index of the "local" interface connecting to
	// the Fuchsia device. nil if this does not apply.
	Interface *net.Interface

	// Error is the error associated with the device when returned via
	// the StartDiscover function.
	Error error
}

// NewClient creates a new Client instance.
func NewClient(timeout time.Duration) *Client {
	return &Client{
		Timeout:    timeout,
		ServerPort: serverPort,
		AdvertPort: advertPort,
		Cookie:     baseCookie,
	}
}

type netbootQuery struct {
	message netbootMessage
	fuchsia bool

	conn6 *ipv6.PacketConn
	conn  *net.UDPConn
	port  int // The port to write on.

	isOpen bool
}

func bindNetbootPort() (*net.UDPConn, error) {
	var err error
	var conn *net.UDPConn
	// https://fuchsia.googlesource.com/fuchsia/+/0e30059/zircon/tools/netprotocol/netprotocol.c#59
	for i := clientPortStart; i <= clientPortEnd; i++ {
		conn, err = net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6zero, Port: i})
		if err == nil {
			break
		}
	}
	return conn, err
}

func newNetbootQuery(nodename string, cookie uint32, port int, fuchsia bool) (*netbootQuery, error) {
	conn, err := bindNetbootPort()
	if err != nil {
		return nil, err
	}
	req := netbootMessage{
		Header: netbootHeader{
			Magic:  magic,
			Cookie: cookie,
			Cmd:    cmdQuery,
			Arg:    0,
		},
	}
	copy(req.Data[:], nodename)
	conn6 := ipv6.NewPacketConn(conn)
	conn6.SetControlMessage(ipv6.FlagDst|ipv6.FlagSrc|ipv6.FlagInterface, true)
	return &netbootQuery{
		message: req,
		fuchsia: fuchsia,
		conn:    conn,
		conn6:   conn6,
		port:    port,
		isOpen:  true}, nil
}

func (n *netbootQuery) write() error {
	// Cleanup function is used here in favor of defer to be explicit about
	// what is being returned. It is difficult to reason about otherwise.
	cleanup := func(e error) error {
		if e != nil {
			n.conn.Close()
		}
		return e
	}
	var buf bytes.Buffer
	if err := binary.Write(&buf, binary.LittleEndian, n.message); err != nil {
		return cleanup(err)
	}
	ifaces, err := net.Interfaces()
	if err != nil {
		return cleanup(err)
	}
	wrote := false
	// Tracks last write error (in the event that all writes fail, for debugging).
	var lastWriteErr error
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		if iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			return cleanup(err)
		}

		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			if ip == nil || ip.To16() == nil {
				continue
			}

			_, err := n.conn.WriteToUDP(buf.Bytes(), &net.UDPAddr{
				IP:   net.IPv6linklocalallnodes,
				Port: n.port,
				Zone: iface.Name,
			})
			// Skip errors here, as it may be possible to write on
			// some interfaces but not others. Track last error in
			// case all writes fail on all interfaces.
			if err != nil {
				lastWriteErr = err
				continue
			}
			wrote = true
		}
	}
	if !wrote {
		return cleanup(fmt.Errorf("write on any iface. Last err: %v", lastWriteErr))
	}
	return nil
}

func (n *netbootQuery) read() (*Target, error) {
	b := make([]byte, 4096)
	_, cm, _, err := n.conn6.ReadFrom(b)
	// If there was an error, as the connection was already valid at the time
	// of creation, this means that some timeout somewhere else has closed
	// before this function was called (this is going to be called in a loop
	// in a goroutine in most cases). There is no way to determine if the
	// connection is still open unless there is an attempt at reading on it
	// unfortunately.
	if err != nil {
		n.isOpen = false
		return nil, nil
	}
	node, err := n.parse(b)
	if err != nil {
		return nil, err
	}
	if len(node) == 0 {
		return nil, nil
	}
	if n.fuchsia {
		// The netstack link-local address has 11th byte always set to 0xff, set
		// this byte to transform netsvc address to netstack address if needed.
		cm.Src[11] = 0xff
		cm.Dst[11] = 0xff
	}

	var iface *net.Interface
	if cm.IfIndex > 0 {
		iface, err = net.InterfaceByIndex(cm.IfIndex)
		if err != nil {
			return nil, fmt.Errorf("query iface lookup: err")
		}
	}
	return &Target{
		Nodename:      node,
		TargetAddress: cm.Src,
		HostAddress:   cm.Dst,
		Interface:     iface,
	}, nil
}

func (n *netbootQuery) parse(b []byte) (string, error) {
	r := bytes.NewReader(b)
	var res netbootMessage
	if err := binary.Read(r, binary.LittleEndian, &res); err != nil {
		return "", fmt.Errorf("query parse error: %v", err)
	}
	if res.Header.Magic != n.message.Header.Magic || res.Header.Cookie != n.message.Header.Cookie || res.Header.Cmd != cmdAck {
		return "", nil
	}
	data, err := netbootString(res.Data[:])
	if err != nil {
		return "", err
	}
	return data, nil
}

func (n *netbootQuery) close() error {
	return n.conn.Close()
}

// Discover resolves the address of host and returns either the netsvc or
// Fuchsia address dependending on the value of fuchsia.
func (n *Client) Discover(ctx context.Context, nodename string, fuchsia bool) (*net.UDPAddr, error) {
	ctx, cancel := context.WithTimeout(ctx, n.Timeout)
	defer cancel()
	t := make(chan *Target)
	cleanup, err := n.StartDiscover(t, nodename, fuchsia)
	if err != nil {
		return nil, err
	}
	defer cleanup()
	for {
		select {
		case <-ctx.Done():
			return nil, fmt.Errorf("timed out waiting for results")
		case target := <-t:
			if err := target.Error; err != nil {
				return nil, err
			}
			ifaceName := ""
			if target.Interface != nil {
				ifaceName = target.Interface.Name
			}
			return &net.UDPAddr{IP: target.TargetAddress, Zone: ifaceName}, nil
			continue
		}
	}
}

// DiscoverAll attempts to discover all Fuchsia targets on the network. Returns
// either the netsvc address or the Fuchsia address depending on the |fuchsia|
// parameter.
//
// If no devices are found, returns a nil array.
func (n *Client) DiscoverAll(ctx context.Context, fuchsia bool) ([]*Target, error) {
	ctx, cancel := context.WithTimeout(ctx, n.Timeout)
	defer cancel()
	t := make(chan *Target)
	cleanup, err := n.StartDiscover(t, NodenameWildcard, fuchsia)
	if err != nil {
		return nil, err
	}
	defer cleanup()
	results := []*Target{}
	for {
		select {
		case <-ctx.Done():
			if len(results) == 0 {
				return nil, nil
			}
			return results, nil
		case target := <-t:
			// If there's an error and devices hav already been found,
			// this isn't an error.
			if err := target.Error; err != nil && len(results) == 0 {
				return nil, err
			}
			if err := target.Error; err == nil {
				results = append(results, target)
			}
		}
	}
}

// StartDiscover takes a channel and returns every Target as it is found. Returns
// a cleanup function for closing the discovery connection or an error if there
// is a failure.
//
// Errors within discovery will be propagated up the channel via the Target.Error
// field.
//
// The Timeout field is not used with this function, and as such it is the
// caller's responsibility to handle timeouts when using this function.
//
// Example:
//	ctx, cancel := context.WithTimeout(context.Background(), timeout)
//	defer cancel()
//	cleanup, err := c.StartDiscover(t, true)
//	if err != nil {
//		return err
//	}
//	defer cleanup()
//	for {
//		select {
//			case target := <-t:
//				// Do something with the target.
//			case <-ctx.Done():
//				// Do something now that the parent context is
//				// completed.
//		}
//	}
func (n *Client) StartDiscover(t chan<- *Target, nodename string, fuchsia bool) (func() error, error) {
	n.Cookie++
	q, err := newNetbootQuery(nodename, n.Cookie, n.ServerPort, fuchsia)
	if err != nil {
		return nil, err
	}
	go func() {
		defer q.close()
		if err := q.write(); err != nil {
			t <- &Target{Error: err}
			return
		}

		for {
			// Simple cleanup to avoid extra cycles.
			if !q.isOpen {
				return
			}
			target, err := q.read()
			if err != nil {
				t <- &Target{Error: err}
				return
			}
			if target != nil {
				// Only skip if there's a name mismatch.
				if nodename != NodenameWildcard && !strings.Contains(target.Nodename, nodename) {
					continue
				}
				t <- target
			}
		}
	}()
	return q.close, nil
}

func (n *Client) beacon(conn *net.UDPConn) (*net.UDPAddr, error) {
	conn.SetReadDeadline(time.Now().Add(n.Timeout))

	b := make([]byte, 4096)
	_, addr, err := conn.ReadFromUDP(b)
	if err != nil {
		return nil, err
	}

	r := bytes.NewReader(b)
	var res netbootMessage
	if err := binary.Read(r, binary.LittleEndian, &res); err != nil {
		return nil, err
	}

	data, err := netbootString(res.Data[:])
	if err != nil {
		return nil, err
	}
	// The query packet payload contains fields separated by ;.
	for _, f := range strings.Split(string(data[:]), ";") {
		// The field has a key=value format.
		vars := strings.SplitN(f, "=", 2)
		// The field with the "nodename" key contains the name of the device.
		if vars[0] == "nodename" {
			return addr, nil
		}
	}

	return nil, errors.New("no valid beacon")
}

// BeaconOnInterface receives the beacon packet on a particular interface.
func (n *Client) BeaconOnInterface(networkInterface string) (*net.UDPAddr, error) {
	conn, err := net.ListenUDP("udp6", &net.UDPAddr{
		IP:   net.IPv6zero,
		Port: n.AdvertPort,
		Zone: networkInterface,
	})
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	return n.beacon(conn)
}

// Beacon receives the beacon packet, returning the address of the sender.
func (n *Client) Beacon() (*net.UDPAddr, error) {
	conn, err := net.ListenUDP("udp6", &net.UDPAddr{
		IP:   net.IPv6zero,
		Port: n.AdvertPort,
	})
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	return n.beacon(conn)
}

// Boot sends a boot packet to the address.
func (n *Client) Boot(addr *net.UDPAddr) error {
	n.Cookie++
	msg := &netbootHeader{
		Magic:  magic,
		Cookie: n.Cookie,
		Cmd:    cmdBoot,
		Arg:    0,
	}
	if err := sendPacket(msg, addr, n.ServerPort); err != nil {
		return fmt.Errorf("send boot command: %v\n", err)
	}
	return nil
}

// Reboot sends a reboot packet the address.
func (n *Client) Reboot(addr *net.UDPAddr) error {
	n.Cookie++
	msg := &netbootHeader{
		Magic:  magic,
		Cookie: n.Cookie,
		Cmd:    cmdReboot,
		Arg:    0,
	}
	if err := sendPacket(msg, addr, n.ServerPort); err != nil {
		return fmt.Errorf("send reboot command: %v\n", err)
	}
	return nil
}

func sendPacket(msg *netbootHeader, addr *net.UDPAddr, port int) error {
	if msg == nil {
		return errors.New("no message provided")
	}
	var buf bytes.Buffer
	if err := binary.Write(&buf, binary.LittleEndian, *msg); err != nil {
		return err
	}

	conn, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6zero})
	if err != nil {
		return fmt.Errorf("create a socket: %v\n", err)
	}
	defer conn.Close()

	_, err = conn.WriteToUDP(buf.Bytes(), &net.UDPAddr{
		IP:   addr.IP,
		Port: port,
		Zone: addr.Zone,
	})
	return err
}

func netbootString(bs []byte) (string, error) {
	for i, b := range bs {
		if b == 0 {
			return string(bs[:i]), nil
		}
	}
	return "", errors.New("no null terminated string found")
}
