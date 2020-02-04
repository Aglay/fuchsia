// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package eth

import (
	"fmt"
	"reflect"
	"sync"
	"syscall/zx"
	"syscall/zx/zxwait"
	"unsafe"

	"netstack/link"
	"syslog"

	"fidl/fuchsia/hardware/ethernet"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/buffer"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
)

// #cgo CFLAGS: -I${SRCDIR}/../../../zircon/public
// #include <zircon/device/ethernet.h>
import "C"

const zxsioEthSignalStatus = zx.SignalUser0
const tag = "eth"

const FifoEntrySize = C.sizeof_struct_eth_fifo_entry

var _ link.Controller = (*Client)(nil)

var _ stack.LinkEndpoint = (*Client)(nil)

// A Client is an ethernet client.
// It connects to a zircon ethernet driver using a FIFO-based protocol.
// The protocol is described in system/fidl/fuchsia-hardware-ethernet/ethernet.fidl.
type Client struct {
	dispatcher stack.NetworkDispatcher
	wg         sync.WaitGroup

	Info ethernet.Info

	device ethernet.Device
	fifos  ethernet.Fifos
	path   string

	mu        sync.Mutex
	state     link.State
	stateFunc func(link.State)
	arena     *Arena

	rxQueue []C.struct_eth_fifo_entry
	tx      struct {
		mu struct {
			sync.Mutex
			tmp      []C.struct_eth_fifo_entry
			buf      []C.struct_eth_fifo_entry
			inFlight uint32
		}
	}
}

// NewClient creates a new ethernet Client.
func NewClient(clientName string, topo string, device ethernet.Device, arena *Arena) (*Client, error) {
	if status, err := device.SetClientName(clientName); err != nil {
		return nil, err
	} else if err := checkStatus(status, "SetClientName"); err != nil {
		return nil, err
	}
	// TODO(NET-57): once we support IGMP, don't automatically set multicast promisc true
	if status, err := device.ConfigMulticastSetPromiscuousMode(true); err != nil {
		return nil, err
	} else if err := checkStatus(status, "ConfigMulticastSetPromiscuousMode"); err != nil {
		// Some drivers - most notably virtio - don't support this setting.
		if err.(*zx.Error).Status != zx.ErrNotSupported {
			return nil, err
		}
		_ = syslog.WarnTf(tag, "%s", err)
	}
	info, err := device.GetInfo()
	if err != nil {
		return nil, err
	}
	status, fifos, err := device.GetFifos()
	if err != nil {
		return nil, err
	} else if err := checkStatus(status, "GetFifos"); err != nil {
		return nil, err
	}

	c := &Client{
		Info:   info,
		device: device,
		fifos:  *fifos,
		path:   topo,
		arena:  arena,
		// TODO: use 2x depth so that the driver always has rx buffers available. Be careful to
		// preserve the invariant: len(rxQueue) >= cap(rxQueue) - RxDepth.
		rxQueue: make([]C.struct_eth_fifo_entry, 0, fifos.RxDepth),
	}
	for i := 0; i < cap(c.rxQueue); i++ {
		b := arena.alloc(c)
		if b == nil {
			return nil, fmt.Errorf("%s: failed to allocate initial RX buffer %d/%d", tag, i, cap(c.rxQueue))
		}
		c.rxQueue = append(c.rxQueue, arena.entry(b))
	}
	c.tx.mu.buf = make([]C.struct_eth_fifo_entry, 0, fifos.TxDepth)
	c.tx.mu.tmp = make([]C.struct_eth_fifo_entry, 0, fifos.TxDepth)

	c.mu.Lock()
	defer c.mu.Unlock()
	if err := func() error {
		h, err := c.arena.iovmo.Handle().Duplicate(zx.RightSameRights)
		if err != nil {
			return fmt.Errorf("%s: failed to duplicate vmo: %s", tag, err)
		}
		if status, err := device.SetIoBuffer(zx.VMO(h)); err != nil {
			return err
		} else if err := checkStatus(status, "SetIoBuffer"); err != nil {
			return err
		}
		return nil
	}(); err != nil {
		_ = c.closeLocked()
		return nil, err
	}

	return c, nil
}

func (c *Client) MTU() uint32 { return c.Info.Mtu }

func (c *Client) Capabilities() stack.LinkEndpointCapabilities {
	// TODO(tamird/brunodalbo): expose hardware offloading capabilities.
	return 0
}

func (c *Client) MaxHeaderLength() uint16 {
	return 0
}

func (c *Client) LinkAddress() tcpip.LinkAddress {
	return tcpip.LinkAddress(c.Info.Mac.Octets[:])
}

func (c *Client) write(buffer tcpip.PacketBuffer) *tcpip.Error {
	var buf Buffer
	for {
		if buf = c.AllocForSend(); buf != nil {
			break
		}
		if err := c.WaitSend(); err != nil {
			_ = syslog.VLogTf(syslog.DebugVerbosity, tag, "wait error: %s", err)
			return tcpip.ErrWouldBlock
		}
	}
	used := 0
	used += copy(buf[used:], buffer.Header.View())
	for _, v := range buffer.Data.Views() {
		used += copy(buf[used:], v)
	}
	if err := c.Send(buf[:used]); err != nil {
		_ = syslog.VLogTf(syslog.DebugVerbosity, tag, "send error: %s", err)
		return tcpip.ErrWouldBlock
	}

	_ = syslog.VLogTf(syslog.TraceVerbosity, tag, "write=%d", used)

	return nil
}

func (c *Client) WritePacket(_ *stack.Route, _ *stack.GSO, _ tcpip.NetworkProtocolNumber, pkt tcpip.PacketBuffer) *tcpip.Error {
	return c.write(pkt)
}

func (c *Client) WritePackets(r *stack.Route, gso *stack.GSO, pkts []tcpip.PacketBuffer, protocol tcpip.NetworkProtocolNumber) (int, *tcpip.Error) {
	// TODO(tamird/stijlist): do better batching here. We can allocate a buffer for each packet
	// before attempting to send them all to the drive all at once.
	var n int
	for _, pkt := range pkts {
		if err := c.WritePacket(r, gso, protocol, pkt); err != nil {
			return n, err
		}
		n++
	}
	return n, nil
}

func (c *Client) WriteRawPacket(vv buffer.VectorisedView) *tcpip.Error {
	return c.write(tcpip.PacketBuffer{
		Data: vv,
	})
}

func (c *Client) Attach(dispatcher stack.NetworkDispatcher) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		if err := func() error {
			for {
				if len(c.rxQueue) != 0 {
					status, sent := fifoWrite(c.fifos.Rx, c.rxQueue)
					switch status {
					case zx.ErrOk:
					default:
						return &zx.Error{Status: status, Text: "fifoWrite(RX)"}
					}
					c.rxQueue = append(c.rxQueue[:0], c.rxQueue[sent:]...)
				}

				for {
					signals := zx.Signals(zx.SignalFIFOReadable | zx.SignalFIFOPeerClosed | zxsioEthSignalStatus)
					if len(c.rxQueue) != 0 {
						signals |= zx.SignalFIFOWritable
					}
					obs, err := zxwait.Wait(c.fifos.Rx, signals, zx.TimensecInfinite)
					if err != nil {
						return err
					}
					if obs&zxsioEthSignalStatus != 0 {
						if status, err := c.GetStatus(); err != nil {
							_ = syslog.WarnTf(tag, "status error: %s", err)
						} else {
							_ = syslog.VLogTf(syslog.TraceVerbosity, tag, "status: %d", status)

							c.mu.Lock()
							switch status {
							case LinkDown:
								c.changeStateLocked(link.StateDown)
							case LinkUp:
								c.changeStateLocked(link.StateStarted)
							}
							c.mu.Unlock()
						}
					}
					if obs&(zx.SignalFIFOReadable) != 0 {
						dst := c.rxQueue[len(c.rxQueue):cap(c.rxQueue)]
						switch status, received := fifoRead(c.fifos.Rx, dst); status {
						case zx.ErrOk:
							c.rxQueue = c.rxQueue[:uint32(len(c.rxQueue))+received]
							for i, entry := range dst[:received] {
								var emptyLinkAddress tcpip.LinkAddress
								dispatcher.DeliverNetworkPacket(c, emptyLinkAddress, emptyLinkAddress, 0, tcpip.PacketBuffer{
									Data: append(buffer.View(nil), c.arena.bufferFromEntry(entry)...).ToVectorisedView(),
								})
								// This entry is going back to the driver; it can be reused.
								dst[i].length = bufferSize
							}
						default:
							return &zx.Error{Status: status, Text: "fifoRead(RX)"}
						}
					}
					if obs&(zx.SignalFIFOWritable) != 0 {
						break
					}
				}
			}
		}(); err != nil {
			_ = syslog.WarnTf(tag, "RX loop: %s", err)
		}
	}()

	c.dispatcher = dispatcher
}

func (c *Client) IsAttached() bool {
	return c.dispatcher != nil
}

// Wait implements stack.LinkEndpoint. It blocks until an error in the dispatch
// goroutine(s) spawned in Attach occurs.
func (c *Client) Wait() {
	c.wg.Wait()
}

func checkStatus(status int32, text string) error {
	if status := zx.Status(status); status != zx.ErrOk {
		return &zx.Error{Status: status, Text: text}
	}
	return nil
}

func (c *Client) SetOnStateChange(f func(link.State)) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.stateFunc = f
}

func (c *Client) Path() string {
	return c.path
}

func (c *Client) changeStateLocked(s link.State) {
	if s != c.state {
		c.state = s
		if fn := c.stateFunc; fn != nil {
			fn(s)
		}
	}
}

// Up enables the interface.
func (c *Client) Up() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.state != link.StateStarted {
		if status, err := c.device.Start(); err != nil {
			return err
		} else if err := checkStatus(status, "Start"); err != nil {
			return err
		}
		if status, err := c.GetStatus(); err != nil {
			return err
		} else {
			switch status {
			case LinkDown:
				c.changeStateLocked(link.StateDown)
			case LinkUp:
				c.changeStateLocked(link.StateStarted)
			}
		}
	}

	return nil
}

// Down disables the interface.
func (c *Client) Down() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.state != link.StateDown {
		if err := c.device.Stop(); err != nil {
			return err
		}
		c.changeStateLocked(link.StateDown)
	}
	return nil
}

// Close closes a Client, releasing any held resources.
func (c *Client) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.closeLocked()
}

func (c *Client) closeLocked() error {
	if c.state == link.StateClosed {
		return nil
	}
	err := c.device.Stop()
	if err != nil {
		err = fmt.Errorf("fuchsia.hardware.ethernet.Device.Stop() for path %q failed: %s", c.path, err)
	}

	if err := c.fifos.Tx.Close(); err != nil {
		_ = syslog.WarnTf(tag, "failed to close tx fifo: %s", err)
	}
	if err := c.fifos.Rx.Close(); err != nil {
		_ = syslog.WarnTf(tag, "failed to close rx fifo: %s", err)
	}
	c.arena.freeAll(c)
	c.changeStateLocked(link.StateClosed)

	return err
}

func (c *Client) SetPromiscuousMode(enabled bool) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if status, err := c.device.SetPromiscuousMode(enabled); err != nil {
		return err
	} else if err := checkStatus(status, "SetPromiscuousMode"); err != nil {
		return err
	}
	return nil
}

// AllocForSend returns a Buffer to be passed to Send.
// If there are too many outstanding transmission buffers, then
// AllocForSend will return nil. WaitSend can be called to block
// until a transmission buffer is available.
func (c *Client) AllocForSend() Buffer {
	c.tx.mu.Lock()
	defer c.tx.mu.Unlock()
	// TODO: Use more than txDepth here. More like 2x.
	//       We cannot have more than txDepth outstanding for the fifo,
	//       but we can have more between AllocForSend and Send. When
	//       this is the entire netstack, we want a lot of buffer.
	//       But there is missing tooling here. In particular, calling
	//       Send won't 'release' the buffer, we need an extra step
	//       for that, because we will want to keep the buffer around
	//       until the ACK comes back.
	if c.tx.mu.inFlight == c.fifos.TxDepth {
		return nil
	}
	buf := c.arena.alloc(c)
	if buf != nil {
		c.tx.mu.inFlight++
	}
	return buf
}

// Send sends a Buffer to the ethernet driver.
// Send does not block.
// If the client is closed, Send returns zx.ErrPeerClosed.
func (c *Client) Send(b Buffer) error {
	c.tx.mu.Lock()
	defer c.tx.mu.Unlock()
	if err := c.txCompleteLocked(); err != nil {
		return err
	}
	c.tx.mu.buf = append(c.tx.mu.buf, c.arena.entry(b))

	switch status, count := fifoWrite(c.fifos.Tx, c.tx.mu.buf); status {
	case zx.ErrOk:
		n := copy(c.tx.mu.buf, c.tx.mu.buf[count:])
		c.tx.mu.buf = c.tx.mu.buf[:n]
	case zx.ErrShouldWait:
	default:
		return &zx.Error{Status: status, Text: "eth.Client.RX"}
	}

	return nil
}

// Free frees a Buffer obtained from Recv.
//
// TODO: c.Free(c.AllocForSend()) will leak txInFlight and eventually jam
//       up the client. This is not an expected use of this library, but
//       tracking to handle it could be useful for debugging.
func (c *Client) Free(b Buffer) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.arena.free(c, b)
}

func fifoWrite(handle zx.Handle, b []C.struct_eth_fifo_entry) (zx.Status, uint32) {
	var actual uint
	data := unsafe.Pointer((*reflect.SliceHeader)(unsafe.Pointer(&b)).Data)
	status := zx.Sys_fifo_write(handle, C.sizeof_struct_eth_fifo_entry, data, uint(len(b)), &actual)
	return status, uint32(actual)
}

func fifoRead(handle zx.Handle, b []C.struct_eth_fifo_entry) (zx.Status, uint32) {
	var actual uint
	data := unsafe.Pointer((*reflect.SliceHeader)(unsafe.Pointer(&b)).Data)
	status := zx.Sys_fifo_read(handle, C.sizeof_struct_eth_fifo_entry, data, uint(len(b)), &actual)
	return status, uint32(actual)
}

// txCompleteLocked should be called with the tx lock held.
func (c *Client) txCompleteLocked() error {
	buf := c.tx.mu.tmp[:c.fifos.TxDepth]

	switch status, count := fifoRead(c.fifos.Tx, buf); status {
	case zx.ErrOk:
		c.tx.mu.inFlight -= count
		for _, entry := range buf[:count] {
			c.arena.free(c, c.arena.bufferFromEntry(entry))
		}
	case zx.ErrShouldWait:
	default:
		return &zx.Error{Status: status, Text: "eth.Client.TX"}
	}

	return nil
}

// WaitSend blocks until it is possible to allocate a send buffer,
// or the client is closed.
func (c *Client) WaitSend() error {
	for {
		c.tx.mu.Lock()
		err := c.txCompleteLocked()
		canSend := c.tx.mu.inFlight < c.fifos.TxDepth
		c.tx.mu.Unlock()
		if err != nil {
			return err
		}
		if canSend {
			return nil
		}
		// Errors from waiting handled in txComplete.
		zxwait.Wait(c.fifos.Tx, zx.SignalFIFOReadable|zx.SignalFIFOPeerClosed, zx.TimensecInfinite)
	}
}

// ListenTX tells the ethernet driver to reflect all transmitted
// packets back to this ethernet client.
func (c *Client) ListenTX() error {
	if status, err := c.device.ListenStart(); err != nil {
		return err
	} else if err := checkStatus(status, "ListenStart"); err != nil {
		return err
	}
	return nil
}

type LinkStatus uint32

const (
	LinkDown LinkStatus = 0
	LinkUp   LinkStatus = 1
)

// GetStatus returns the underlying device's status.
func (c *Client) GetStatus() (LinkStatus, error) {
	status, err := c.device.GetStatus()
	return LinkStatus(status & ethernet.DeviceStatusOnline), err
}
