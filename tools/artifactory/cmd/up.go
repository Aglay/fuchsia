// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can
// found in the LICENSE file.

package main

import (
	"compress/gzip"
	"context"
	"crypto/md5"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"

	"go.fuchsia.dev/fuchsia/tools/artifactory/lib"
	"go.fuchsia.dev/fuchsia/tools/build/lib"
	"go.fuchsia.dev/fuchsia/tools/lib/iomisc"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"

	"cloud.google.com/go/storage"
	"github.com/google/subcommands"
)

const (
	// The size in bytes at which files will be read and written to GCS.
	chunkSize = 8 * 1024 * 1024

	// Relative path within the build directory to the repo produced by a build.
	repoSubpath = "amber-files"
	// Names of the repository metadata, key, and blob directories within a repo.
	metadataDirName = "repository"
	keyDirName      = "keys"
	blobDirName     = "blobs"
	imageDirName    = "images"
	debugDirName    = "debug"

	// A record of all of the fuchsia debug symbols processed.
	// This is eventually consumed by crash reporting infrastructure.
	buildIDsTxt = "build-ids.txt"
)

type upCommand struct {
	// GCS bucket to which build artifacts will be uploaded.
	gcsBucket string
	// UUID under which to index artifacts.
	uuid string
	// The maximum number of concurrent uploading routines.
	j int
}

func (upCommand) Name() string { return "up" }

func (upCommand) Synopsis() string { return "upload artifacts from a build to Google Cloud Storage" }

func (upCommand) Usage() string {
	return `
artifactory up -bucket $GCS_BUCKET -uuid $UUID <build directory>

Uploads artifacts from a build to $GCS_BUCKET with the following structure:

├── $GCS_BUCKET
│   │   ├── blobs
│   │   │   └── <blob names>
│   │   ├── $UUID
│   │   │   ├── repository
│   │   │   │   └── <package repo metadata files>
│   │   │   ├── keys
│   │   │   │   └── <package repo keys>
│   │   │   ├── images
│   │   │   │   └── <images>

flags:

`
}

func (cmd *upCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&cmd.gcsBucket, "bucket", "", "GCS bucket to which artifacts will be uploaded")
	f.StringVar(&cmd.uuid, "uuid", "", "UUID under which to index uploaded artifacts")
	f.IntVar(&cmd.j, "j", 500, "maximum number of concurrent uploading processes")
}

func (cmd upCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	args := f.Args()
	if len(args) != 1 {
		logger.Errorf(ctx, "exactly one positional argument expected: the build directory root")
		return subcommands.ExitFailure
	}

	if err := cmd.execute(ctx, args[0]); err != nil {
		logger.Errorf(ctx, "%v", err)
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}

func (cmd upCommand) execute(ctx context.Context, buildDir string) error {
	if cmd.gcsBucket == "" {
		return fmt.Errorf("-bucket is required")
	} else if cmd.uuid == "" {
		return fmt.Errorf("-uuid is required")
	}

	m, err := build.NewModules(buildDir)
	if err != nil {
		return err
	}

	sink, err := newCloudSink(ctx, cmd.gcsBucket)
	if err != nil {
		return err
	}
	defer sink.client.Close()

	repo := path.Join(buildDir, repoSubpath)
	metadataDir := path.Join(repo, metadataDirName)
	keyDir := path.Join(repo, keyDirName)
	blobDir := path.Join(metadataDir, blobDirName)

	dirs := []artifactory.Upload{
		{
			Source:      blobDir,
			Destination: blobDirName,
			Deduplicate: true,
		},
		{
			Source:      metadataDir,
			Destination: path.Join(cmd.uuid, metadataDirName),
			Deduplicate: false,
		},
		{
			Source:      keyDir,
			Destination: path.Join(cmd.uuid, keyDirName),
			Deduplicate: false,
		},
	}

	var files []artifactory.Upload

	images := artifactory.ImageUploads(m, path.Join(cmd.uuid, imageDirName))
	files = append(files, images...)

	debugBinaries, buildIDs, err := artifactory.DebugBinaryUploads(m, debugDirName)
	if err != nil {
		return err
	}
	files = append(files, debugBinaries...)
	buildIDManifest, err := createBuildIDManifest(buildIDs)
	if err != nil {
		return err
	}
	defer os.Remove(buildIDManifest)
	files = append(files, artifactory.Upload{
		Source:      buildIDManifest,
		Destination: path.Join(cmd.uuid, buildIDsTxt),
	})

	for _, dir := range dirs {
		contents, err := dirToFiles(ctx, dir)
		if err != nil {
			return err
		}
		files = append(files, contents...)
	}
	return uploadFiles(ctx, files, sink, cmd.j)
}

func createBuildIDManifest(buildIDs []string) (string, error) {
	manifest, err := ioutil.TempFile("", buildIDsTxt)
	if err != nil {
		return "", err
	}
	defer manifest.Close()
	_, err = io.WriteString(manifest, strings.Join(buildIDs, "\n"))
	return manifest.Name(), err
}

// DataSink is an abstract data sink, providing a mockable interface to
// cloudSink, the GCS-backed implementation below.
type dataSink interface {

	// ObjectExistsAt returns whether an object of that name exists within the sink.
	objectExistsAt(context.Context, string) (bool, error)

	// Write writes the content of a file to a sink object at the given name.
	// If an object at that name does not exists, it will be created; else it
	// will be overwritten. If the written object has a checksum differing from
	// the provided checksum, then an error will be returned, as this might
	// derive from an opaque server-side error).
	write(context.Context, string, string, []byte) error
}

// CloudSink is a GCS-backed data sink.
type cloudSink struct {
	client *storage.Client
	bucket *storage.BucketHandle
}

func newCloudSink(ctx context.Context, bucket string) (*cloudSink, error) {
	client, err := storage.NewClient(ctx)
	if err != nil {
		return nil, err
	}
	return &cloudSink{
		client: client,
		bucket: client.Bucket(bucket),
	}, nil
}

func (s cloudSink) objectExistsAt(ctx context.Context, name string) (bool, error) {
	_, err := s.bucket.Object(name).Attrs(ctx)
	if err == storage.ErrObjectNotExist {
		return false, nil
	} else if err != nil {
		return false, fmt.Errorf("object %q: possibly exists remotely, but is in an unknown state: %v", name, err)
	}
	return true, nil
}

func (s cloudSink) write(ctx context.Context, name string, path string, expectedChecksum []byte) error {
	obj := s.bucket.Object(name)
	w := obj.If(storage.Conditions{DoesNotExist: true}).NewWriter(ctx)
	w.ChunkSize = chunkSize
	w.MD5 = expectedChecksum
	w.ContentType = "application/octet-stream"
	w.ContentEncoding = "gzip"

	fd, err := os.Open(path)
	if err != nil {
		return err
	}
	defer fd.Close()

	gzw := gzip.NewWriter(w)
	// The following writer is effectively |gzw|, but for which |w| is also
	// closed on Close(). Both need to be closed to finalize the write of the
	// compressed file to GCS.
	zw := struct {
		io.Writer
		io.Closer
	}{gzw, iomisc.MultiCloser(gzw, w)}
	defer zw.Close()
	return artifactory.Copy(ctx, name, fd, zw, chunkSize)
}

type checksumError struct {
	name     string
	expected []byte
	actual   []byte
}

func (err checksumError) Error() string {
	return fmt.Sprintf(
		"object %q: checksum mismatch: expected %v; actual %v",
		err.name, err.expected, err.actual,
	)
}

// dirToFiles returns a list of the top-level files in the dir.
func dirToFiles(ctx context.Context, dir artifactory.Upload) ([]artifactory.Upload, error) {
	var files []artifactory.Upload
	entries, err := ioutil.ReadDir(dir.Source)
	if os.IsNotExist(err) {
		logger.Debugf(ctx, "%s does not exist; skipping upload", dir.Source)
		return nil, nil
	} else if err != nil {
		return nil, err
	}
	for _, fi := range entries {
		if fi.IsDir() {
			continue
		}
		files = append(files, artifactory.Upload{
			Source:      filepath.Join(dir.Source, fi.Name()),
			Destination: filepath.Join(dir.Destination, fi.Name()),
			Deduplicate: dir.Deduplicate,
		})
	}
	return files, nil
}

func uploadFiles(ctx context.Context, files []artifactory.Upload, dest dataSink, j int) error {
	if j <= 0 {
		return fmt.Errorf("Concurrency factor j must be a positive number")
	}

	uploads := make(chan artifactory.Upload, j)
	errs := make(chan error, j)

	queueUploads := func() {
		defer close(uploads)
		for _, f := range files {
			if _, err := os.Stat(f.Source); err != nil {
				// The associated artifacts might not actually have been created, which is valid.
				if os.IsNotExist(err) {
					logger.Debugf(ctx, "%s does not exist; skipping upload", f.Source)
					continue
				}
				errs <- err
				return
			}
			uploads <- f
		}
	}

	var wg sync.WaitGroup
	wg.Add(j)
	upload := func() {
		defer wg.Done()
		for upload := range uploads {
			exists, err := dest.objectExistsAt(ctx, upload.Destination)
			if err != nil {
				errs <- err
				return
			}
			if exists {
				logger.Debugf(ctx, "object %q: already exists remotely", upload.Destination)
				if !upload.Deduplicate {
					errs <- fmt.Errorf("object %q: collided", upload.Destination)
					return
				}
				continue
			}

			checksum, err := md5Checksum(upload.Source)
			if err != nil {
				errs <- err
				return
			}

			if err := dest.write(ctx, upload.Destination, upload.Source, checksum); err != nil {
				errs <- fmt.Errorf("%s: %v", upload.Destination, err)
				return
			}
			logger.Debugf(ctx, "object %q: created", upload.Destination)
		}
	}

	go queueUploads()
	for i := 0; i < j; i++ {
		go upload()
	}
	wg.Wait()
	close(errs)
	return <-errs
}

// Determines the checksum of the associated gzipped file without reading all
// of the contents into memory.
func md5Checksum(file string) ([]byte, error) {
	fd, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer fd.Close()

	h := md5.New()
	gzw := gzip.NewWriter(h)
	if _, err := io.Copy(gzw, fd); err != nil {
		gzw.Close()
		return nil, err
	}
	if err := gzw.Close(); err != nil {
		return nil, err
	}
	checksum := h.Sum(nil)
	return checksum[:], nil
}
