// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package botanist

import (
	"bytes"
	"context"
	"fmt"
	"strings"

	"go.fuchsia.dev/fuchsia/src/sys/pkg/lib/repo"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/net/sshutil"
)

const (
	repoID               = "fuchsia-pkg://fuchsia.com"
	localhostPlaceholder = "localhost"
)

// AddPackageRepository adds a package repository to a connected fuchsia
// instance with the provided metadata and blob URLs.
// In either URL, a host of "localhost" will be resolved and scoped as
// appropriate when dealing with the address from the host and target
// perspectives.
func AddPackageRepository(ctx context.Context, client *sshutil.Client, repoURL, blobURL string) error {
	localhost := strings.Contains(repoURL, localhostPlaceholder) || strings.Contains(blobURL, localhostPlaceholder)

	lScopedRepoURL := repoURL
	if localhost {
		host := localScopedLocalHost(client.LocalAddr().String())
		lScopedRepoURL = strings.Replace(repoURL, localhostPlaceholder, host, 1)
		logger.Infof(ctx, "local-scoped package repository address: %s\n", lScopedRepoURL)
	}

	rScopedRepoURL := repoURL
	rScopedBlobURL := blobURL
	if localhost {
		host, err := remoteScopedLocalHost(ctx, client)
		if err != nil {
			return err
		}
		rScopedRepoURL = strings.Replace(repoURL, localhostPlaceholder, host, 1)
		logger.Infof(ctx, "remote-scoped package repository address: %s\n", rScopedRepoURL)
		rScopedBlobURL = strings.Replace(blobURL, localhostPlaceholder, host, 1)
		logger.Infof(ctx, "remote-scoped package blob address: %s\n", rScopedBlobURL)
	}

	rootMeta, err := repo.GetRootMetadataInsecurely(ctx, lScopedRepoURL)
	if err != nil {
		return fmt.Errorf("failed to derive root metadata: %w", err)
	}

	cfg := &repo.Config{
		URL:           repoID,
		RootKeys:      rootMeta.RootKeys,
		RootVersion:   rootMeta.RootVersion,
		RootThreshold: rootMeta.RootThreshold,
		Mirrors: []repo.MirrorConfig{
			{
				URL:     rScopedRepoURL,
				BlobURL: rScopedBlobURL,
			},
		},
	}

	return repo.AddFromConfig(ctx, client, cfg)
}

func localScopedLocalHost(laddr string) string {
	tokens := strings.Split(laddr, ":")
	host := strings.Join(tokens[:len(tokens)-1], ":") // Strips the port.
	return escapePercentSign(host)
}

func remoteScopedLocalHost(ctx context.Context, client *sshutil.Client) (string, error) {
	// From the ssh man page:
	// "SSH_CONNECTION identifies the client and server ends of the connection.
	// The variable contains four space-separated values: client IP address,
	// client port number, server IP address, and server port number."
	// We wish to obtain the client IP address, as will be scoped from the
	// remote address.
	var stdout bytes.Buffer
	if err := client.Run(ctx, []string{"echo", "${SSH_CONNECTION}"}, &stdout, nil); err != nil {
		return "", fmt.Errorf("failed to derive $SSH_CONNECTION: %w", err)
	}
	val := string(stdout.Bytes())
	tokens := strings.Split(val, " ")
	if len(tokens) != 4 {
		return "", fmt.Errorf("$SSH_CONNECTION should be four space-separated values and not %q", val)
	}
	host := escapePercentSign("[" + tokens[0] + "]")
	return host, nil
}

// From the spec https://tools.ietf.org/html/rfc6874#section-2:
// "%" is always treated as an escape character in a URI, so, according to
// the established URI syntax any occurrences of literal "%" symbols in a
// URI MUST be percent-encoded and represented in the form "%25".
func escapePercentSign(addr string) string {
	if strings.Contains(addr, "%25") {
		return addr
	}
	return strings.Replace(addr, "%", "%25", 1)
}
