package pkgfs

import (
	"io"
	"path/filepath"
	"strings"
	"time"

	"fidl/fuchsia/mem"

	"go.fuchsia.dev/fuchsia/garnet/go/src/far"

	"syscall/zx"
	zxio "syscall/zx/io"

	"thinfs/fs"
)

func newMetaFar(blob string, fs *Filesystem) *metaFar {
	return &metaFar{
		blob: blob,
		fs:   fs,
	}
}

// metaFar is a shared reference to an open meta.far or one or more of it's contents.
type metaFar struct {
	blob string

	fs *Filesystem
}

func (mf *metaFar) open() (*far.Reader, error) {
	f, err := mf.fs.blobfs.Open(mf.blob)
	if err != nil {
		return nil, err
	}

	fr, err := far.NewReader(f)
	if err != nil {
		f.Close()
	}
	return fr, err
}

func (mf *metaFar) list() ([]string, error) {
	fr, err := mf.open()
	if err != nil {
		return nil, err
	}
	defer fr.Close()
	return fr.List(), nil
}

// metaFile is the package dir "meta" opened as a file, which on read returns
// the merkleroot.
type metaFile struct {
	unsupportedFile

	*metaFar

	off   int64
	flags fs.OpenFlags
}

func newMetaFile(blob string, fs *Filesystem, flags fs.OpenFlags) *metaFile {
	return &metaFile{
		unsupportedFile("package/meta:" + blob),
		newMetaFar(blob, fs),
		0,
		flags,
	}
}

func (f *metaFile) Close() error {
	return nil
}

func (f *metaFile) GetOpenFlags() fs.OpenFlags {
	return f.flags
}

func (f *metaFile) Stat() (int64, time.Time, time.Time, error) {
	return int64(len(f.blob)), time.Time{}, time.Time{}, nil
}

func (f *metaFile) Read(p []byte, off int64, whence int) (int, error) {
	if whence != fs.WhenceFromCurrent {
		return 0, fs.ErrNotSupported
	}
	if f.off+off >= int64(len(f.blob)) {
		return 0, fs.ErrEOF
	}

	n := copy(p, f.blob[f.off+off:])
	f.off += off + int64(n)
	return n, nil
}

var _ fs.Directory = (*metaFarDir)(nil)

type metaFarDir struct {
	unsupportedDirectory

	*metaFar

	path string
}

func newMetaFarDir(blob string, fs *Filesystem) *metaFarDir {
	return &metaFarDir{
		unsupportedDirectory("package/meta:" + blob),
		newMetaFar(blob, fs),
		"meta",
	}
}

func newMetaFarDirAt(blob string, fs *Filesystem, path string) *metaFarDir {
	mf := newMetaFarDir(blob, fs)
	mf.path = path
	return mf
}

func (d *metaFarDir) Close() error {

	return nil
}

func (d *metaFarDir) Dup() (fs.Directory, error) {
	return d, nil
}

func (d *metaFarDir) Open(name string, flags fs.OpenFlags) (fs.File, fs.Directory, *fs.Remote, error) {
	name = clean(name)

	// Nothing in the meta directory is ever executable.
	if flags.Execute() {
		return nil, nil, nil, fs.ErrNotSupported
	}

	if name == "" {
		if flags.File() || (!flags.Directory() && !flags.Path()) {
			return newMetaFile(d.blob, d.fs, flags), nil, nil, nil
		}
		return nil, d, nil, nil
	}

	name = filepath.Join(d.path, name)

	if flags.Create() || flags.Truncate() || flags.Write() || flags.Append() {

		return nil, nil, nil, fs.ErrNotSupported
	}

	contents, err := d.metaFar.list()
	if err != nil {
		return nil, nil, nil, err
	}

	for _, lname := range contents {
		if name == lname {
			mff, err := newMetaFarFile(d.blob, d.fs, name)
			return mff, nil, nil, err
		}
	}

	dname := name + "/"
	for _, lname := range contents {
		if strings.HasPrefix(lname, dname) {
			return nil, newMetaFarDirAt(d.blob, d.fs, name), nil, nil
		}
	}

	return nil, nil, nil, fs.ErrNotFound
}

func (d *metaFarDir) Read() ([]fs.Dirent, error) {
	contents, err := d.metaFar.list()
	if err != nil {
		return nil, goErrToFSErr(err)
	}

	// TODO(raggi): improve efficiency
	dirs := map[string]struct{}{}
	dents := []fs.Dirent{}
	dents = append(dents, dirDirEnt("."))

	for _, name := range contents {
		if !strings.HasPrefix(name, d.path+"/") {
			continue
		}
		name = strings.TrimPrefix(name, d.path+"/")

		parts := strings.SplitN(name, "/", 2)
		if len(parts) == 2 {
			if _, ok := dirs[parts[0]]; !ok {
				dirs[parts[0]] = struct{}{}
				dents = append(dents, dirDirEnt(parts[0]))
			}

		} else {
			dents = append(dents, fileDirEnt(parts[0]))
		}
	}
	return dents, nil
}

func (d *metaFarDir) Stat() (int64, time.Time, time.Time, error) {
	// TODO(raggi): forward stat values from the index
	contents, err := d.metaFar.list()
	if err != nil {
		return 0, time.Time{}, time.Time{}, goErrToFSErr(err)
	}
	return int64(len(contents)), d.fs.mountTime, d.fs.mountTime, nil
}

var _ fs.File = (*metaFarFile)(nil)
var _ fs.FileWithGetBuffer = (*metaFarFile)(nil)

type metaFarFile struct {
	unsupportedFile

	*metaFar
	fr *far.Reader
	er io.ReaderAt

	off  int64
	path string
	vmo  *zx.VMO
}

func newMetaFarFile(blob string, fs *Filesystem, path string) (*metaFarFile, error) {
	mf := newMetaFar(blob, fs)
	fr, err := mf.open()
	if err != nil {
		return nil, goErrToFSErr(err)
	}
	er, err := fr.Open(path)
	if err != nil {
		fr.Close()
		return nil, goErrToFSErr(err)
	}

	return &metaFarFile{
		unsupportedFile("package/meta:" + blob + "/" + path),
		mf,
		fr,
		er,
		0,
		path,
		nil,
	}, nil
}

func (f *metaFarFile) Close() error {
	if f.vmo != nil {
		f.vmo.Close()
	}
	f.fr.Close()
	return nil
}

func (f *metaFarFile) Dup() (fs.File, error) {
	fr, err := f.metaFar.open()
	if err != nil {
		return nil, goErrToFSErr(err)
	}
	er, err := fr.Open(f.path)
	if err != nil {
		fr.Close()
		return nil, goErrToFSErr(err)
	}

	return &metaFarFile{
		f.unsupportedFile,
		f.metaFar,
		fr,
		er,
		0,
		f.path,
		nil,
	}, nil
}

func (f *metaFarFile) Read(p []byte, off int64, whence int) (int, error) {
	// TODO(raggi): this could allocate less/be far more efficient

	switch whence {
	case fs.WhenceFromCurrent:
		f.off += off
		n, err := f.er.ReadAt(p, f.off)
		f.off += int64(n)
		return n, goErrToFSErr(err)
	case fs.WhenceFromStart:
		return f.er.ReadAt(p, off)
	}
	return 0, fs.ErrNotSupported
}

func (f *metaFarFile) Seek(offset int64, whence int) (int64, error) {
	var err error
	var n int64
	switch whence {
	case fs.WhenceFromCurrent:
		f.off = f.off + offset
	case fs.WhenceFromStart:
		f.off = offset
	case fs.WhenceFromEnd:
		err = fs.ErrNotSupported
	default:
		return 0, fs.ErrInvalidArgs
	}
	if err != nil {
		return f.off, goErrToFSErr(err)
	}
	return n, nil
}

func (f *metaFarFile) Stat() (int64, time.Time, time.Time, error) {
	return int64(f.fr.GetSize(f.path)), time.Time{}, time.Time{}, nil
}

func (f *metaFarFile) GetBuffer(flags uint32) (*mem.Buffer, error) {
	size := f.fr.GetSize(f.path)
	if f.vmo == nil {
		// TODO(52938): This could be implemented more efficiently by creating a
		// child VMO of the blob's VMO with the VM_SLICE_CHILD_NO_WRITE option.
		// The FAR format ensures each file will be at a 4KB offset into the
		// containing blob.
		vmo, err := zx.NewVMO(size, zx.VMOOption(0))
		if err != nil {
			return nil, fs.ErrNoSpace
		}
		buf := make([]byte, size)
		n, err := f.er.ReadAt(buf, 0)
		if n != int(size) {
			return nil, fs.ErrIO
		}
		err = vmo.Write(buf, 0)
		if err != nil {
			return nil, fs.ErrIO
		}
		f.vmo = &vmo
	}

	rights := zx.RightsBasic | zx.RightMap | zx.RightsProperty

	if flags&zxio.VmoFlagRead != 0 {
		rights |= zx.RightRead
	}
	if flags&zxio.VmoFlagWrite != 0 {
		// Contents of a meta directory are never writable.
		return nil, fs.ErrReadOnly
	}
	if flags&zxio.VmoFlagExec != 0 {
		// Contents of a meta directory are never executable.
		return nil, fs.ErrPermission
	}

	if flags&zxio.VmoFlagExact != 0 {
		return nil, fs.ErrNotSupported
	}

	buffer := &mem.Buffer{}
	buffer.Size = size

	if flags&zxio.VmoFlagPrivate != 0 {
		// Create a separate VMO for the caller if they specified that they want a private copy.

		options := zx.VMOChildOption(zx.VMOChildOptionSnapshotAtLeastOnWrite)
		options |= zx.VMOChildOptionNoWrite
		offset := uint64(0)
		child, err := f.vmo.CreateChild(options, offset, size)
		if err != nil {
			return nil, fs.ErrIO
		}
		buffer.Vmo = child
	} else {
		// Otherwise, just duplicate our VMO.

		handle := zx.Handle(*f.vmo)
		d, err := handle.Duplicate(rights)
		if err != nil {
			return nil, fs.ErrPermission
		}
		buffer.Vmo = zx.VMO(d)
	}

	return buffer, nil
}
