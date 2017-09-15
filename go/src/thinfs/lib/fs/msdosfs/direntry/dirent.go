// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Package direntry describes FAT directory entries.
package direntry

import (
	"bytes"
	"time"

	"github.com/golang/glog"

	"fuchsia.googlesource.com/thinfs/lib/fs"
)

// GetDirentryCallback returns the directory entry at an index.
// The index is relative to the start of the directory.
//
// This function should only need to be called once for short direntries.
//
// For long direntries, it may need to be called multiple times to read the long direntry
// components.
type GetDirentryCallback func(uint) ([]byte, error)

// Dirent describes an in-memory representation of a direntry.
// This must be an in-memory representation, rather than one which accesses persistant storage, as
// it gets passed back to the user when "fs.Directory.Read()" is called.
type Dirent struct {
	Cluster   uint32    // Cluster equals zero if we're opening the root directory
	Filename  string    // A UTF-8 encoded representation of the filename
	Size      uint32    // The size field includes all dirents up to and including the "last free" dirent
	WriteTime time.Time // The last time the file was modified

	attributes uint8 // Never set to "long"
	nameDOS    []byte
	free       bool
	lastFree   bool
}

// Ensure the Dirent implements the fs.Dirent interface
var _ fs.Dirent = (*Dirent)(nil)

// New creates a new in-memory Dirent.
//
// Does not allocate any on-disk space; simply creates an in-memory structure.
func New(name string, cluster uint32, attr fs.FileType) *Dirent {
	glog.V(1).Info("Making a new dirent with name: ", name)
	d := &Dirent{}
	// Set filename. The "short" or "long"-ness of the name should not matter (yet).
	// As a consequence, the filename is not actually validated until it is serialized.
	d.Filename = name

	// Set cluster.
	d.Cluster = cluster
	d.WriteTime = time.Now() // File creation is considered a write
	// "size" can be set to zero. File and directories start with zero size.

	// Set Attributes
	switch attr {
	case fs.FileTypeDirectory:
		d.attributes |= attrDirectory
	case fs.FileTypeRegularFile:
		d.attributes |= attrNormal
	}

	return d
}

// LookupDirent looks for (and loads) a dirent with a given name, along with the direntryIndex
// at which it was found.
//
// If a Dirent with a matching name is not found, return "nil".
func LookupDirent(callback GetDirentryCallback, name string) (*Dirent, uint, error) {
	glog.V(1).Info("Looking up a dirent with name: ", name)
	for direntryIndex := uint(0); ; {
		d, numSlots, err := LoadDirent(callback, direntryIndex)
		if err != nil {
			glog.V(2).Infof("At index %d, encountered an error", direntryIndex)
			return nil, 0, err
		} else if d.IsLastFree() {
			glog.V(2).Infof("At index %d, encountered the last free direntry", direntryIndex)
			return nil, 0, nil
		} else if d.IsFree() {
			glog.V(2).Infof("At index %d, encountered a free direntry", direntryIndex)
			direntryIndex++
			continue
		}

		glog.V(2).Infof("At index %d, encountered direntry named %s", direntryIndex, d.Filename)
		if d.Filename == name {
			return d, direntryIndex, nil
		}
		glog.V(2).Infof("Name not found. Jumping %d slots", numSlots)
		direntryIndex += uint(numSlots)
	}
}

// doesShortNameExist is a helper function wrapped around the callback. Helps determine if a short
// entry name exists in the directory.
func doesShortNameExist(callback GetDirentryCallback, shortName []byte) (bool, error) {
	for i := uint(0); ; i++ {
		buf, err := callback(i)
		if err != nil {
			return false, err
		}
		short := makeShort(buf)
		if short.isLastFree() {
			// We can't read any more direntries
			break
		} else if short.isFree() || (short.attributes&attrLongname == attrLongname) {
			// This particular direntry is not a short name
			continue
		}

		if bytes.Equal(short.nameRaw(), shortName) {
			return true, nil
		}
	}
	return false, nil
}

// LoadDirent converts a buffer of bytes (from disk) to an in-memory dirent.
// Returns the number of direntry slots required to represent the logical Dirent.
//
// If the direntry is a "short" filename (or free entry), the index points to it directly.
//
// If the direntry is a "long" filename, the index points to the first long entry, which precedes
// (optional) other long entries and (required) a short entry.
//
// Otherwise, the requested direntry is invalid.
func LoadDirent(callback GetDirentryCallback, direntryIndex uint) (*Dirent, uint8, error) {
	glog.V(1).Info("Loading a dirent")
	buf, err := callback(direntryIndex)
	if err != nil {
		return nil, 0, err
	}

	// It is not yet possible to identify if the loaded dirent is short or long.
	// Assume short, then read the attributes.
	short := makeShort(buf)

	// If the dirent is free, then a short dirent with "is free" set to true will be returned.
	if short.attributes&attrLongname != attrLongname {
		glog.V(2).Info("Loaded dirent is short")
		// The direntry is actually with a short filename.
		return &Dirent{
			Cluster:    short.cluster(),
			Filename:   short.name(),
			Size:       short.size(),
			WriteTime:  short.lastUpdateTime(),
			attributes: short.attributes,
			free:       short.isFree(),
			lastFree:   short.isLastFree(),
		}, 1, nil
	}
	glog.V(2).Info("Loaded dirent is long")

	// The direntry is associated with a long filename.
	buf, numDirentrySlots, err := getShortEntryFromWin(callback, direntryIndex)
	if err != nil {
		return nil, 0, err
	}
	short = makeShort(buf)
	if short.isFree() {
		// Long direntry is associated with a free shortname -- therefore, the long direntry has
		// been deleted.
		return nil, 0, errLongDirentry
	}
	unixName, err := convertWinToUnix(callback, direntryIndex, checksum(short.nameRaw()), numDirentrySlots-1)
	if err != nil {
		return nil, 0, err
	}
	return &Dirent{
		Cluster:    short.cluster(),
		Filename:   unixName,
		Size:       short.size(),
		WriteTime:  short.lastUpdateTime(),
		attributes: short.attributes,
		free:       short.isFree(),
		lastFree:   short.isLastFree(),
	}, numDirentrySlots, nil
}

// GetName implements fs.Dirent
func (d *Dirent) GetName() string {
	return d.Filename
}

// GetType implements fs.Dirent
func (d *Dirent) GetType() fs.FileType {
	switch d.attributes & (attrDirectory | attrArchive) {
	case 0, attrArchive:
		return fs.FileTypeRegularFile
	case attrDirectory:
		return fs.FileTypeDirectory
	default:
		return fs.FileTypeUnknown
	}
}

// IsFree returns true if the Dirent represents a free spot.
func (d *Dirent) IsFree() bool {
	return d.free || d.lastFree
}

// IsLastFree returns true if the Dirent represents the last free spot.
func (d *Dirent) IsLastFree() bool {
	return d.lastFree
}

// Serialize converts the in-memory "Dirent" to a "disk-ready" byte slice.
// The result should be a multiple of "DirentrySize" bytes long.
func (d *Dirent) Serialize(callback GetDirentryCallback) ([]byte, error) {
	var result []byte

	// Since we are preparing to put this file in storage, we need to ensure it has a unique
	// generation number that distinguishes it from other files with the same prefix.
	nameDOS, longnameNeeded, err := convertUnixToDOS(callback, d.Filename)
	if err != nil {
		return nil, err
	}

	// Convert Dirent to a short direntry (this is necessary for both short and long names)
	shortDirent := &shortDirentry{
		attributes: d.attributes,
	}
	shortDirent.setName(nameDOS)
	shortDirent.setCluster(d.Cluster)
	shortDirent.setLastUpdateTime(d.WriteTime)
	shortDirent.setSize(d.Size)

	// Prepend all long direntries, if necessary
	if longnameNeeded {
		longDirents, err := convertUnixToWin(d.Filename, nameDOS)
		if err != nil {
			return nil, err
		}
		for i := range longDirents {
			result = append(result, longDirents[i].bytes()...)
		}
	}

	// Convert shortDirent struct to byte slice
	result = append(result, shortDirent.bytes()...)
	return result, nil
}

// LastFreeDirent creates a byte slice representing "Last free directory entry".
func LastFreeDirent() []byte {
	shortDirent := &shortDirentry{}
	shortDirent.setLastFree()
	buf := shortDirent.bytes()
	return buf
}

// FreeDirent creates a byte slice representing "Free directory entry".
func FreeDirent() []byte {
	shortDirent := &shortDirentry{}
	shortDirent.setFree()
	return shortDirent.bytes()
}
