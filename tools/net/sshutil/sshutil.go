// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package sshutil

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"net"
	"time"

	"go.fuchsia.dev/fuchsia/tools/lib/retry"
	"go.fuchsia.dev/fuchsia/tools/net/netutil"

	"golang.org/x/crypto/ssh"
)

const (
	// Default SSH server port.
	SSHPort = 22

	// Default RSA key size.
	RSAKeySize = 2048

	// The allowed timeout for a single attempt at establishing an SSH
	// connection.
	connectAttemptTimeout = 10 * time.Second

	// The allowed timeout to establish an ssh connection, possibly including
	// many attempts.
	totalConnectTimeout = 2 * time.Minute

	sshUser = "fuchsia"
)

var (
	// defaultConnectBackoff is the connection backoff for clients returned by
	// Connect() and ConnectToNode().
	// NOTE: This retry strategy was somewhat arbitrarily chosen and can be
	// changed if there's a compelling reason to choose a different strategy.
	defaultConnectBackoff = retry.WithMaxDuration(&retry.ZeroBackoff{}, totalConnectTimeout)
)

// ConnectionError is an all-purpose error indicating that a client has become
// unresponsive.
type ConnectionError struct {
	Err error
}

func (e ConnectionError) Unwrap() error {
	return e.Err
}

func (e ConnectionError) Error() string {
	// ConnectionError is intended to be an umbrella error type for all kinds of
	// SSH-related errors, so there's no information we can add to the
	// underlying error message that would be particularly useful in all
	// scenarios.
	if e.Err != nil {
		return e.Err.Error()
	}
	return "SSH connection error"
}

// IsConnectionError determines whether the given error is a ConnectionError.
// This is a common check that we include in sshutil to save callers a line of
// code.
func IsConnectionError(err error) bool {
	var connErr ConnectionError
	return errors.As(err, &connErr)
}

// GeneratePrivateKey generates a private SSH key.
func GeneratePrivateKey() ([]byte, error) {
	key, err := rsa.GenerateKey(rand.Reader, RSAKeySize)
	if err != nil {
		return nil, err
	}
	privateKey := &pem.Block{
		Type:  "RSA PRIVATE KEY",
		Bytes: x509.MarshalPKCS1PrivateKey(key),
	}
	buf := pem.EncodeToMemory(privateKey)

	return buf, nil
}

// ConnectToNode connects to the device with the given nodename.
func ConnectToNode(ctx context.Context, nodename string, config *ssh.ClientConfig) (*Client, error) {
	addr, err := netutil.GetNodeAddress(ctx, nodename, true)
	if err != nil {
		return nil, err
	}
	addr.Port = SSHPort
	return NewClient(ctx, addr, config, defaultConnectBackoff)
}

// DefaultSSHConfig returns a basic SSH client configuration.
func DefaultSSHConfig(privateKey []byte) (*ssh.ClientConfig, error) {
	signer, err := ssh.ParsePrivateKey(privateKey)
	if err != nil {
		return nil, err
	}
	return DefaultSSHConfigFromSigners(signer)
}

// DefaultSSHConfigFromSigners returns a basic SSH client configuration.
func DefaultSSHConfigFromSigners(signers ...ssh.Signer) (*ssh.ClientConfig, error) {
	return &ssh.ClientConfig{
		User:            sshUser,
		Auth:            []ssh.AuthMethod{ssh.PublicKeys(signers...)},
		Timeout:         connectAttemptTimeout,
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
	}, nil
}

// Returns the network to use to SSH into a device.
func network(address net.Addr) (string, error) {
	var ip *net.IP

	// We need these type assertions because the net package (annoyingly) doesn't provide
	// an interface for objects that have an IP address.
	switch addr := address.(type) {
	case *net.UDPAddr:
		ip = &addr.IP
	case *net.TCPAddr:
		ip = &addr.IP
	case *net.IPAddr:
		ip = &addr.IP
	default:
		return "", fmt.Errorf("unsupported address type: %T", address)
	}

	if ip.To4() != nil {
		return "tcp", nil // IPv4
	}
	if ip.To16() != nil {
		return "tcp6", nil // IPv6
	}
	return "", fmt.Errorf("cannot infer network for IP address %s", ip.String())
}
