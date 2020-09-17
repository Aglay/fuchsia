// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package checklicenses

import (
	"fmt"
)

// Walk gathers all Licenses then checks for a match within each filtered file
func Walk(config *Config) error {
	metrics := new(Metrics)
	metrics.Init()
	file_tree := NewFileTree(config, metrics)
	licenses, err := NewLicenses(config.LicensePatternDir)
	if err != nil {
		return err
	}
	for tree := range file_tree.getSingleLicenseFileIterator() {
		for singleLicenseFile := range tree.singleLicenseFiles {
			if err := processSingleLicenseFile(singleLicenseFile, metrics, licenses, config, tree); err != nil {
				// error safe to ignore because eg. io.EOF means symlink hasn't been handled yet
				fmt.Printf("warning: %s. Skipping file: %s.\n", err, tree.getPath())
			}
		}
	}
	for path := range file_tree.getFileIterator() {
		if err := processFile(path, metrics, licenses, config, file_tree); err != nil {
			// error safe to ignore because eg. io.EOF means symlink hasn't been handled yet
			fmt.Printf("warning: %s. Skipping file: %s.\n", err, path)
		}
	}
	file, err := createOutputFile(config)
	if err != nil {
		return err
	}
	saveToOutputFile(file, licenses, config, metrics)
	metrics.print()
	return nil
}

func processSingleLicenseFile(base string, metrics *Metrics, licenses *Licenses, config *Config, file_tree *FileTree) error {
	// TODO(solomonkinard) larger limit for single license files?
	path := file_tree.getPath() + "/" + base
	data, err := readFromFile(path, config.MaxReadSize)
	if err != nil {
		return err
	}
	licenses.MatchSingleLicenseFile(data, base, metrics, file_tree)
	return nil
}

func processFile(path string, metrics *Metrics, licenses *Licenses, config *Config, file_tree *FileTree) error {
	fmt.Printf("visited file or dir: %q\n", path)
	data, err := readFromFile(path, config.MaxReadSize)
	if err != nil {
		return err
	}
	is_matched := licenses.MatchFile(data, path, metrics)
	if !is_matched {
		project := file_tree.getProjectLicense(path)
		if project == nil {
			metrics.increment("num_unlicensed")
			fmt.Printf("File license: missing. Project license: missing. path: %s\n", path)
		} else {
			metrics.increment("num_with_project_license")
			for _, arr_license := range project.singleLicenseFiles {
				for i, license := range arr_license {
					if i == 0 {
						metrics.increment("num_one_file_matched_to_one_single_license")
					}
					fmt.Printf("project license: %s\n", license.category)
					metrics.increment("num_one_file_matched_to_multiple_single_licenses")
					licenses.MatchAuthors([]byte(license.pattern.String()), path, license)
				}
			}
			fmt.Printf("File license: missing. Project license: exists. path: %s\n", path)
		}
	}
	return nil
}

// TODO(solomonkinard) tools/zedmon/client/pubspec.yaml" error reading: EOF
