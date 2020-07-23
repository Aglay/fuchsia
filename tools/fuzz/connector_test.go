// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fuzz

import (
	"io/ioutil"
	"os"
	"path"
	"path/filepath"
	"testing"

	"github.com/golang/glog"
	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
)

// These SSHConnector tests connect to an in-memory SSH server (see ssh_mock.go
// for details), so we have good coverage of the SSH/SFTP mechanics. However,
// on the remote side, they rely on mocked commands and a mocked filesystem so
// do not test interaction with an actual instance.  For that, we rely on the
// end-to-end tests in e2e_test.go.

func TestSSHConnectorHandle(t *testing.T) {
	c := &SSHConnector{Host: "somehost", Port: 123, Key: "keyfile"}

	handle, err := NewHandleFromObjects(c)
	if err != nil {
		t.Fatalf("error creating handle: %s", err)
	}

	// Note: we don't serialize here because that is covered by handle tests
	reloadedConn, err := loadConnectorFromHandle(handle)
	if err != nil {
		t.Fatalf("error loading connector from handle: %s", err)
	}

	c2, ok := reloadedConn.(*SSHConnector)
	if !ok {
		t.Fatalf("incorrect connector type")
	}

	if diff := cmp.Diff(c, c2, cmpopts.IgnoreUnexported(SSHConnector{})); diff != "" {
		t.Fatalf("incorrect data in reloaded connector (-want +got):\n%s", diff)
	}
}

func TestSSHCommand(t *testing.T) {
	c, _ := getFakeSSHConnector(t)
	defer c.Close()

	arg := "some cool args"
	cmd := c.Command("echo", arg)
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("error running remote command: %s", err)
	}

	if string(out) != arg+"\n" {
		t.Fatalf("unexpected output: %q", string(out))
	}
}

func TestSSHInvalidCommand(t *testing.T) {
	c, _ := getFakeSSHConnector(t)
	defer c.Close()

	cmd := c.Command("LOAD", `"*",8`)
	if err := cmd.Run(); err == nil || err.(*InstanceCmdError).ReturnCode != 127 {
		t.Fatalf("expected command not found but got: %s", err)
	}
}

func TestSSHGet(t *testing.T) {
	c, fs := getFakeSSHConnector(t)
	defer c.Close()

	tmpDir := getTempdir(t)
	defer os.RemoveAll(tmpDir)

	testFile := &fakeFile{name: "/testfile", content: "test file contents"}
	fs.files = append(fs.files, testFile)

	if err := c.Get("/testfile", tmpDir); err != nil {
		t.Fatalf("error getting file: %s", err)
	}

	got, err := ioutil.ReadFile(path.Join(tmpDir, testFile.name))
	if err != nil {
		t.Fatalf("error reading fetched file: %s", err)
	}

	if diff := cmp.Diff(testFile.content, string(got)); diff != "" {
		t.Fatalf("fetched file has unexpected content (-want +got):\n%s", diff)
	}
}

func TestSSHGetNonexistentFile(t *testing.T) {
	c, _ := getFakeSSHConnector(t)
	defer c.Close()

	tmpDir := getTempdir(t)
	defer os.RemoveAll(tmpDir)

	if err := c.Get("/testfile", tmpDir); err == nil {
		t.Fatal("expected error but succeeded")
	}
}

func TestSSHPut(t *testing.T) {
	c, fs := getFakeSSHConnector(t)
	defer c.Close()

	tmpDir := getTempdir(t)
	defer os.RemoveAll(tmpDir)

	tmpFile := path.Join(tmpDir, "testfile")
	fileContents := "test file contents"

	if err := ioutil.WriteFile(tmpFile, []byte(fileContents), 0644); err != nil {
		t.Fatalf("error writing local file: %s", err)
	}

	remotePath := "/some/dir"
	if err := c.Put(tmpFile, remotePath); err != nil {
		t.Fatalf("error putting file: %s", err)
	}

	expectRemoteFileWithContent(t, fs, path.Join(remotePath, filepath.Base(tmpFile)), fileContents)
}

func TestSSHGetGlob(t *testing.T) {
	c, fs := getFakeSSHConnector(t)
	defer c.Close()

	tmpDir := getTempdir(t)
	defer os.RemoveAll(tmpDir)

	testFiles := []*fakeFile{
		{name: "/subdir/a", content: "apple"},
		{name: "/subdir/b", content: "banana"},
		{name: "/subdir/j", content: "jabuticaba"},
	}

	// Add a fake directory entry so globbing works correctly
	fs.files = append(testFiles, &fakeFile{name: "/subdir", isDir: true})

	if err := c.Get("/subdir/*", tmpDir); err != nil {
		t.Fatalf("error running remote command: %s", err)
	}

	for _, testFile := range testFiles {
		got, err := ioutil.ReadFile(path.Join(tmpDir, filepath.Base(testFile.name)))
		if err != nil {
			t.Fatalf("error reading fetched file: %s", err)
		}

		if diff := cmp.Diff(testFile.content, string(got)); diff != "" {
			t.Fatalf("fetched file has unexpected content (-want +got):\n%s", diff)
		}
	}
}

func TestSSHPutGlob(t *testing.T) {
	c, fs := getFakeSSHConnector(t)
	defer c.Close()

	tmpDir := getTempdir(t)
	defer os.RemoveAll(tmpDir)

	testFiles := []*fakeFile{
		{name: "a", content: "apple"},
		{name: "b", content: "banana"},
		{name: "j", content: "jabuticaba"},
	}

	for _, testFile := range testFiles {
		tmpFile := path.Join(tmpDir, testFile.name)
		if err := ioutil.WriteFile(tmpFile, []byte(testFile.content), 0644); err != nil {
			t.Fatalf("error writing local file: %s", err)
		}
	}

	remotePath := "/some/dir"
	if err := c.Put(path.Join(tmpDir, "*"), remotePath); err != nil {
		t.Fatalf("error putting file: %s", err)
	}

	for _, testFile := range testFiles {
		expectRemoteFileWithContent(t, fs, path.Join(remotePath, testFile.name), testFile.content)
	}
}

// Helper functions:

func getFakeSSHConnector(t *testing.T) (*SSHConnector, *fakeSftp) {
	glog.Info("Starting local SSH server...")

	conn, errCh, fakeFs, err := startLocalSSHServer()
	if err != nil {
		t.Fatalf("error starting local server: %s", err)
	}

	// Monitor for server errors
	go func() {
		for err := range errCh {
			t.Errorf("error from local SSH server: %s", err)
		}
	}()

	return conn, fakeFs
}

func expectRemoteFileWithContent(t *testing.T, fs *fakeSftp, name string, content string) {
	for _, f := range fs.files {
		if f.name == name {
			if diff := cmp.Diff(content, f.content); diff != "" {
				t.Fatalf("uploaded file has unexpected content (-want +got):\n%s", diff)
			}
			return
		}
	}
	t.Fatalf("uploaded file not found in expected location")
}
