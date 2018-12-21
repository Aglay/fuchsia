// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package botanist

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// Image represents an entry in an image manifest.
type Image struct {
	// Name is the canonical name of the image.
	Name string `json:"name"`

	// Path is the absolute path to the image.
	// Note: when unmarshalled from a manifest this entry actually gives the relative
	// location from the manifest's directory; we prepend that directory when loading. See
	// LoadImages() below.
	Path string `json:"path"`

	// Type is the shorthand for the type of the image (e.g., "zbi" or "blk").
	Type string `json:"type"`

	// PaveArgs is the list of associated arguments to pass to the bootserver
	// when paving.
	PaveArgs []string `json:"bootserver_pave"`

	// NetbootArgs is the list of associated arguments to pass to the bootserver
	// when netbooting.
	NetbootArgs []string `json:"bootserver_netboot"`
}

// GetImage is a convenience function that returns the first image in a list with the
// given name, or nil if no such image exists.
func GetImage(images []Image, name string) *Image {
	for _, img := range images {
		if img.Name == name {
			return &img
		}
	}
	return nil
}

// LoadImages reads in the entries indexed in the given image manifests.
func LoadImages(imageManifests ...string) ([]Image, error) {
	decodeImages := func(manifest string) ([]Image, error) {
		f, err := os.Open(manifest)
		if err != nil {
			return nil, fmt.Errorf("failed to open %s: %v", manifest, err)
		}
		defer f.Close()
		imgs := []Image{}
		if err := json.NewDecoder(f).Decode(&imgs); err != nil {
			return nil, fmt.Errorf("failed to decode %s: %v", manifest, err)
		}
		manifestDir, err := filepath.Abs(filepath.Dir(manifest))
		if err != nil {
			return nil, err
		}
		for i, _ := range imgs {
			imgs[i].Path = filepath.Join(manifestDir, imgs[i].Path)
		}
		return imgs, nil
	}

	imgs := []Image{}
	for _, manifest := range imageManifests {
		decoded, err := decodeImages(manifest)
		if err != nil {
			return nil, err
		}
		imgs = append(imgs, decoded...)
	}
	return imgs, nil
}
