// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"app/context"
	"fidl/bindings"

	"garnet/public/lib/wlan/fidl/wlan_service"
)

const (
	cmdScan    = "scan"
	cmdConnect = "connect"
)

type ToolApp struct {
	ctx  *context.Context
	wlan *wlan_service.Wlan_Proxy
}

func (a *ToolApp) Scan(seconds uint8) {
	expiry := (time.Duration(seconds) + 5) * time.Second
	t := time.NewTimer(expiry)

	rxed := make(chan struct{})
	go func() {
		res, err := a.wlan.Scan(wlan_service.ScanRequest{seconds})
		if err != nil {
			fmt.Println("Error:", err)
		} else if res.Error.Code != wlan_service.ErrCode_Ok {
			fmt.Println("Error:", res.Error.Description)
		} else {
			for _, ap := range *res.Aps {
				prot := " "
				if ap.IsSecure {
					prot = "*"
				}
				fmt.Printf("%x (RSSI: %d) %v %q\n",
					ap.Bssid, ap.LastRssi, prot, ap.Ssid)
			}
		}
		rxed <- struct{}{}
	}()

	select {
	case <-rxed:
		// Received scan results.
	case <-t.C:
		fmt.Printf("Scan timed out; aborting.\n")
	}

}

func (a *ToolApp) Connect(ssid string, passPhrase string, seconds uint8) {
	if len(ssid) > 32 {
		fmt.Println("ssid is too long")
		return
	}
	werr, err := a.wlan.Connect(wlan_service.ConnectConfig{ssid, passPhrase, seconds})
	if err != nil {
		fmt.Println("Error:", err)
	} else if werr.Code != wlan_service.ErrCode_Ok {
		fmt.Println("Error:", werr.Description)
	}
}

func usage(progname string) {
	fmt.Printf("usage: %v %v [<timeout>]\n", progname, cmdScan)
	fmt.Printf("       %v %v <ssid> [<passphrase> <scan interval>]\n", progname, cmdConnect)
}

func main() {
	a := &ToolApp{ctx: context.CreateFromStartupInfo()}
	r, p := a.wlan.NewRequest(bindings.GetAsyncWaiter())
	a.wlan = p
	defer a.wlan.Close()
	a.ctx.ConnectToEnvService(r)

	if len(os.Args) < 2 {
		usage(os.Args[0])
		return
	}

	cmd := os.Args[1]
	switch cmd {
	case cmdScan:
		if len(os.Args) == 3 {
			i, err := strconv.ParseInt(os.Args[2], 10, 8)
			if err != nil {
				fmt.Println("Error:", err)
			} else {
				a.Scan(uint8(i))
			}
		} else {
			a.Scan(0)
		}
	case cmdConnect:
		if len(os.Args) < 3 || len(os.Args) > 5 {
			usage(os.Args[0])
			return
		}

		ssid := os.Args[2]
		passPhrase := ""
		scanInterval := uint8(0)

		// Read optional arguments. One at a time. If an argument was parsed successfully, proceed in
		// argument list.
		nextArgsIdx := 3
		if len(os.Args) > nextArgsIdx && IsValidPSKPassPhrase(os.Args[nextArgsIdx]) {
			passPhrase = os.Args[nextArgsIdx]
			nextArgsIdx++
		}

		if len(os.Args) > nextArgsIdx {
			i, err := strconv.ParseInt(os.Args[nextArgsIdx], 10, 8)
			if err != nil {
				fmt.Println("Error:", err)
				return
			}
			scanInterval = uint8(i)
			nextArgsIdx++
		}

		// If one of the optional arguments was not parsed correctly, show error.
		if nextArgsIdx != len(os.Args) {
			usage(os.Args[0])
			return
		}
		a.Connect(ssid, passPhrase, scanInterval)
	default:
		usage(os.Args[0])
	}
}

func IsValidPSKPassPhrase(passPhrase string) bool {
	// len(s) can be used because PSK always operates on ASCII characters.
	if len(passPhrase) < 8 || len(passPhrase) > 63 {
		return false
	}
	for _, c := range passPhrase {
		if c < 32 || c > 126 {
			return false
		}
	}
	return true
}
