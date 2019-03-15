// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"time"

	"fuchsia.googlesource.com/tools/mdns"
)

type mDNSResponse struct {
	rxIface  net.Interface
	devAddr  net.Addr
	rxPacket mdns.Packet
}

func (m *mDNSResponse) getReceiveIP() (net.IP, error) {
	if unicastAddrs, err := m.rxIface.Addrs(); err != nil {
		return nil, err
	} else {
		for _, addr := range unicastAddrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			if ip == nil || ip.To4() == nil {
				continue
			}
			return ip, nil
		}
	}
	return nil, fmt.Errorf("no IPv4 unicast addresses found on iface %v", m.rxIface)
}

type mDNSHandler func(mDNSResponse, bool, chan<- *fuchsiaDevice, chan<- error)

// Contains common command information for embedding in other dev_finder commands.
type devFinderCmd struct {
	// Outputs in JSON format if true.
	json bool
	// The mDNS port to connect to.
	mdnsPort int
	// The timeout in ms to either give up or to exit the program after finding at least one
	// device.
	timeout int
	// Determines whether to return the address of the address of the interface that
	// established a connection to the Fuchsia device (rather than the address of the
	// Fuchsia device on its own).
	localResolve bool
	// The limit of devices to discover. If this number of devices has been discovered before
	// the timeout has been reached the program will exit successfully.
	deviceLimit int

	mdnsHandler mDNSHandler
}

type fuchsiaDevice struct {
	addr   net.IP
	domain string
}

func (cmd *devFinderCmd) SetCommonFlags(f *flag.FlagSet) {
	f.BoolVar(&cmd.json, "json", false, "Outputs in JSON format.")
	f.IntVar(&cmd.mdnsPort, "port", 5353, "The port your mDNS servers operate on.")
	f.IntVar(&cmd.timeout, "timeout", 2000, "The number of milliseconds before declaring a timeout.")
	f.BoolVar(&cmd.localResolve, "local", false, "Returns the address of the interface to the host when doing service lookup/domain resolution.")
	f.IntVar(&cmd.deviceLimit, "device-limit", 0, "Exits before the timeout at this many devices per resolution (zero means no limit).")
}

// Extracts the IP from its argument, returning an error if the type is unsupported.
func addrToIP(addr net.Addr) (net.IP, error) {
	switch v := addr.(type) {
	case *net.IPNet:
		return v.IP, nil
	case *net.IPAddr:
		return v.IP, nil
	case *net.UDPAddr:
		return v.IP, nil
	}
	return nil, errors.New("unsupported address type")
}

func (cmd *devFinderCmd) sendMDNSPacket(ctx context.Context, packet mdns.Packet) ([]*fuchsiaDevice, error) {
	if cmd.mdnsHandler == nil {
		return nil, fmt.Errorf("packet handler is nil")
	}
	if cmd.timeout <= 0 {
		return nil, fmt.Errorf("invalid timeout value: %v", cmd.timeout)
	}

	var m mdns.MDNS
	errChan := make(chan error)
	devChan := make(chan *fuchsiaDevice)
	m.AddHandler(func(recv net.Interface, addr net.Addr, rxPacket mdns.Packet) {
		response := mDNSResponse{recv, addr, rxPacket}
		cmd.mdnsHandler(response, cmd.localResolve, devChan, errChan)
	})
	m.AddErrorHandler(func(err error) {
		errChan <- err
	})
	m.AddWarningHandler(func(addr net.Addr, err error) {
		log.Printf("from: %v warn: %v\n", addr, err)
	})
	ctx, cancel := context.WithTimeout(ctx, time.Duration(cmd.timeout)*time.Millisecond)
	defer cancel()
	if err := m.Start(ctx, cmd.mdnsPort); err != nil {
		errChan <- fmt.Errorf("starting mdns: %v", err)
	}
	m.Send(packet)
	devices := make([]*fuchsiaDevice, 0)
	for {
		select {
		case <-ctx.Done():
			if len(devices) == 0 {
				return nil, fmt.Errorf("timeout")
			}
			return devices, nil
		case err := <-errChan:
			return nil, err
		case device := <-devChan:
			devices = append(devices, device)
			if cmd.deviceLimit != 0 && len(devices) == cmd.deviceLimit {
				return devices, nil
			}
		}
	}
}
