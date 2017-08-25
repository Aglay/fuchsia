// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package wlan

import (
	mlme "apps/wlan/services/wlan_mlme"
	mlme_ext "apps/wlan/services/wlan_mlme_ext"
	"apps/wlan/services/wlan_service"

	"fmt"
	"log"
	"time"
)

type state interface {
	run(*Client) (time.Duration, error)
	commandIsDisabled() bool
	handleCommand(r *commandRequest) (state, error)
	handleMLMEMsg(interface{}, *Client) (state, error)
	handleMLMETimeout(*Client) (state, error)
	needTimer() (bool, time.Duration)
	timerExpired(*Client) (state, error)
}

type Command int

const (
	CmdScan Command = iota
)

const InfiniteTimeout = 0 * time.Second

// Scanning

const DefaultScanInterval = 5 * time.Second
const ScanTimeout = 30 * time.Second

type scanState struct {
	scanInterval time.Duration
	pause        bool
	running      bool
	cmdPending   *commandRequest
}

var twoPointFourGhzChannels = []uint16{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}
var broadcastBssid = [6]uint8{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

func newScanState(c *Client) *scanState {
	scanInterval := time.Duration(0)
	pause := true
	if c.cfg != nil {
		scanInterval = DefaultScanInterval
		if c.cfg.ScanInterval > 0 {
			scanInterval = time.Duration(c.cfg.ScanInterval) * time.Second
		}
		if c.cfg.SSID != "" {
			// start periodic scan.
			pause = false
		}
	}

	return &scanState{scanInterval: scanInterval, pause: pause}
}

func (s *scanState) String() string {
	return "scanning"
}

func newScanRequest(ssid string) *mlme.ScanRequest {
	return &mlme.ScanRequest{
		BssType:        mlme.BssTypes_Infrastructure,
		Bssid:          broadcastBssid,
		Ssid:           ssid,
		ScanType:       mlme.ScanTypes_Passive,
		ChannelList:    &twoPointFourGhzChannels,
		MinChannelTime: 100,
		MaxChannelTime: 300,
	}
}

func (s *scanState) run(c *Client) (time.Duration, error) {
	var req *mlme.ScanRequest
	if s.cmdPending != nil && s.cmdPending.id == CmdScan {
		req = newScanRequest("")
	} else if c.cfg != nil && c.cfg.SSID != "" && !s.pause {
		req = newScanRequest(c.cfg.SSID)
	}
	if req != nil {
		if debug {
			log.Printf("scan req: %v timeout: %v", req, ScanTimeout)
		}
		err := c.sendMessage(req, int32(mlme.Method_ScanRequest))
		if err != nil {
			return 0, err
		}
		s.running = true
	}
	return ScanTimeout, nil
}

func (s *scanState) commandIsDisabled() bool {
	return s.running
}

func (s *scanState) handleCommand(cmd *commandRequest) (state, error) {
	switch cmd.id {
	case CmdScan:
		s.cmdPending = cmd
	}
	return s, nil
}

func (s *scanState) handleMLMEMsg(msg interface{}, c *Client) (state, error) {
	switch v := msg.(type) {
	case *mlme.ScanResponse:
		if debug {
			PrintScanResponse(v)
		}
		s.running = false

		if s.cmdPending != nil && s.cmdPending.id == CmdScan {
			aps := CollectScanResults(v, "")
			s.cmdPending.respC <- &CommandResult{aps, nil}
			s.cmdPending = nil
		} else if c.cfg != nil && c.cfg.SSID != "" {
			aps := CollectScanResults(v, c.cfg.SSID)
			if len(aps) > 0 {
				c.ap = &aps[0]
				return newJoinState(), nil
			}
			s.pause = true
		}

		return s, nil

	default:
		return s, fmt.Errorf("unexpected message type: %T", v)
	}
}

func (s *scanState) handleMLMETimeout(c *Client) (state, error) {
	if debug {
		log.Printf("scan timeout")
	}
	s.pause = false
	return s, nil
}

func (s *scanState) needTimer() (bool, time.Duration) {
	if s.running {
		return false, 0
	}
	if debug {
		log.Printf("scan pause %v start", s.scanInterval)
	}
	return true, s.scanInterval
}

func (s *scanState) timerExpired(c *Client) (state, error) {
	if debug {
		log.Printf("scan pause finished")
	}
	s.pause = false
	return s, nil
}

// Joining

type joinState struct {
}

func newJoinState() *joinState {
	return &joinState{}
}

func (s *joinState) String() string {
	return "joining"
}

func (s *joinState) run(c *Client) (time.Duration, error) {
	req := &mlme.JoinRequest{
		SelectedBss:        *c.ap.BSSDesc,
		JoinFailureTimeout: 20,
	}
	if debug {
		log.Printf("join req: %v", req)
	}

	return InfiniteTimeout, c.sendMessage(req, int32(mlme.Method_JoinRequest))
}

func (s *joinState) commandIsDisabled() bool {
	return true
}

func (s *joinState) handleCommand(r *commandRequest) (state, error) {
	return s, nil
}

func (s *joinState) handleMLMEMsg(msg interface{}, c *Client) (state, error) {
	switch v := msg.(type) {
	case *mlme.JoinResponse:
		if debug {
			PrintJoinResponse(v)
		}

		if v.ResultCode == mlme.JoinResultCodes_Success {
			return newAuthState(), nil
		} else {
			return newScanState(c), nil
		}
	default:
		return s, fmt.Errorf("unexpected message type: %T", v)
	}
}

func (s *joinState) handleMLMETimeout(c *Client) (state, error) {
	return s, nil
}

func (s *joinState) needTimer() (bool, time.Duration) {
	return false, 0
}

func (s *joinState) timerExpired(c *Client) (state, error) {
	return s, nil
}

// Authenticating

type authState struct {
}

func newAuthState() *authState {
	return &authState{}
}

func (s *authState) String() string {
	return "authenticating"
}

func (s *authState) run(c *Client) (time.Duration, error) {
	req := &mlme.AuthenticateRequest{
		PeerStaAddress:     c.ap.BSSDesc.Bssid,
		AuthType:           mlme.AuthenticationTypes_OpenSystem,
		AuthFailureTimeout: 20,
	}
	if debug {
		log.Printf("auth req: %v", req)
	}

	return InfiniteTimeout, c.sendMessage(req, int32(mlme.Method_AuthenticateRequest))
}

func (s *authState) commandIsDisabled() bool {
	return true
}

func (s *authState) handleCommand(r *commandRequest) (state, error) {
	return s, nil
}

func (s *authState) handleMLMEMsg(msg interface{}, c *Client) (state, error) {
	switch v := msg.(type) {
	case *mlme.AuthenticateResponse:
		if debug {
			PrintAuthenticateResponse(v)
		}

		if v.ResultCode == mlme.AuthenticateResultCodes_Success {
			return newAssocState(), nil
		} else {
			return newScanState(c), nil
		}
	default:
		return s, fmt.Errorf("unexpected message type: %T", v)
	}
}

func (s *authState) handleMLMETimeout(c *Client) (state, error) {
	return s, nil
}

func (s *authState) needTimer() (bool, time.Duration) {
	return false, 0
}

func (s *authState) timerExpired(c *Client) (state, error) {
	return s, nil
}

// Associating

type assocState struct {
}

func newAssocState() *assocState {
	return &assocState{}
}

func (s *assocState) String() string {
	return "associating"
}

func (s *assocState) run(c *Client) (time.Duration, error) {
	req := &mlme.AssociateRequest{
		PeerStaAddress: c.ap.BSSDesc.Bssid,
	}
	if debug {
		log.Printf("assoc req: %v", req)
	}

	return InfiniteTimeout, c.sendMessage(req, int32(mlme.Method_AssociateRequest))
}

func (s *assocState) commandIsDisabled() bool {
	return true
}

func (s *assocState) handleCommand(r *commandRequest) (state, error) {
	return s, nil
}

func (s *assocState) handleMLMEMsg(msg interface{}, c *Client) (state, error) {
	switch v := msg.(type) {
	case *mlme.AssociateResponse:
		if debug {
			PrintAssociateResponse(v)
		}

		if v.ResultCode == mlme.AssociateResultCodes_Success {
			return newAssociatedState(), nil
		} else {
			return newScanState(c), nil
		}
	default:
		return s, fmt.Errorf("unexpected message type: %T", v)
	}
}

func (s *assocState) handleMLMETimeout(c *Client) (state, error) {
	return s, nil
}

func (s *assocState) needTimer() (bool, time.Duration) {
	return false, 0
}

func (s *assocState) timerExpired(c *Client) (state, error) {
	return s, nil
}

// Associated

type associatedState struct {
}

func newAssociatedState() *associatedState {
	return &associatedState{}
}

func (s *associatedState) String() string {
	return "associated"
}

func (s *associatedState) run(c *Client) (time.Duration, error) {
	return InfiniteTimeout, nil
}

func (s *associatedState) commandIsDisabled() bool {
	// TODO: disable if Scan request is running
	return false
}

func (s *associatedState) handleCommand(r *commandRequest) (state, error) {
	// TODO: handle Scan command
	r.respC <- &CommandResult{nil,
		&wlan_service.Error{
			wlan_service.ErrCode_NotSupported,
			"Can't run the command in associatedState"}}
	return s, nil
}

func (s *associatedState) handleMLMEMsg(msg interface{}, c *Client) (state, error) {
	switch v := msg.(type) {
	case *mlme.DisassociateIndication:
		if debug {
			PrintDisassociateIndication(v)
		}
		return newAssocState(), nil
	case *mlme.DeauthenticateIndication:
		if debug {
			PrintDeauthenticateIndication(v)
		}
		return newAuthState(), nil
	case *mlme_ext.SignalReportIndication:
		if debug {
			PrintSignalReportIndication(v)
		}
		return s, nil
	case *mlme_ext.EapolIndication:
		// TODO(hahnr): Handle EAPOL frame.
		return s, nil
	default:
		return s, fmt.Errorf("unexpected message type: %T", v)
	}
}

func (s *associatedState) handleMLMETimeout(c *Client) (state, error) {
	return s, nil
}

func (s *associatedState) needTimer() (bool, time.Duration) {
	return false, 0
}

func (s *associatedState) timerExpired(c *Client) (state, error) {
	return s, nil
}
