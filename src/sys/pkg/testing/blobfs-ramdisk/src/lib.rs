// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(missing_docs)]

//! Test utilities for starting a blobfs server.

use {
    anyhow::{format_err, Context as _, Error},
    fdio::{SpawnAction, SpawnOptions},
    fidl::endpoints::{ClientEnd, ServerEnd},
    fidl_fuchsia_io::{
        DirectoryAdminMarker, DirectoryAdminProxy, DirectoryMarker, DirectoryProxy, NodeProxy,
    },
    fuchsia_component::server::ServiceFs,
    fuchsia_merkle::Hash,
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon::{self as zx, prelude::*},
    futures::prelude::*,
    ramdevice_client::RamdiskClient,
    std::{collections::BTreeSet, ffi::CString},
};

#[cfg(test)]
mod test;

/// A ramdisk-backed blobfs instance
pub struct BlobfsRamdisk {
    backing_ramdisk: Ramdisk,
    process: KillOnDrop<fuchsia_zircon::Process>,
    proxy: DirectoryAdminProxy,
}

impl BlobfsRamdisk {
    /// Starts a blobfs server backed by a freshly formatted ramdisk.
    pub fn start() -> Result<Self, Error> {
        // make a new ramdisk and format it with blobfs.
        let ramdisk = Ramdisk::start().context("creating backing ramdisk for blobfs")?;
        mkblobfs(&ramdisk)?;

        Self::start_with_ramdisk(ramdisk)
    }

    /// Starts a blobfs server backed by the given ramdisk.
    pub fn start_with_ramdisk(ramdisk: Ramdisk) -> Result<Self, Error> {
        // spawn blobfs on top of the ramdisk.
        let block_device_handle_id = HandleInfo::new(HandleType::User0, 1);
        let fs_root_handle_id = HandleInfo::new(HandleType::User0, 0);

        let block_handle = ramdisk.clone_channel().context("cloning ramdisk channel")?;

        let (proxy, blobfs_server_end) = fidl::endpoints::create_proxy::<DirectoryAdminMarker>()?;
        let process = fdio::spawn_etc(
            &fuchsia_runtime::job_default(),
            SpawnOptions::CLONE_ALL,
            &CString::new("/pkg/bin/blobfs").unwrap(),
            &[&CString::new("blobfs").unwrap(), &CString::new("mount").unwrap()],
            None,
            &mut [
                SpawnAction::add_handle(block_device_handle_id, block_handle.into()),
                SpawnAction::add_handle(fs_root_handle_id, blobfs_server_end.into()),
            ],
        )
        .map_err(|(status, _)| status)
        .context("spawning 'blobfs mount'")?;

        Ok(Self { backing_ramdisk: ramdisk, process: process.into(), proxy })
    }

    /// Returns a new connection to blobfs's root directory as a raw zircon channel.
    pub fn root_dir_handle(&self) -> Result<ClientEnd<DirectoryMarker>, Error> {
        let (root_clone, server_end) = zx::Channel::create()?;
        self.proxy.clone(fidl_fuchsia_io::CLONE_FLAG_SAME_RIGHTS, server_end.into())?;
        Ok(root_clone.into())
    }

    /// Returns a new connection to blobfs's root directory as a DirectoryProxy.
    pub fn root_dir_proxy(&self) -> Result<DirectoryProxy, Error> {
        Ok(self.root_dir_handle()?.into_proxy()?)
    }

    /// Returns a new connetion to blobfs's root directory as a openat::Dir.
    pub fn root_dir(&self) -> Result<openat::Dir, Error> {
        use std::os::unix::io::{FromRawFd, IntoRawFd};

        let f = fdio::create_fd(self.root_dir_handle()?.into()).unwrap();

        let dir = {
            let fd = f.into_raw_fd();
            // Convert our raw file descriptor into an openat::Dir. This is enclosed in an unsafe
            // block because a RawFd may or may not be owned, and it is only safe to construct an
            // owned handle from an owned RawFd, which this does.
            unsafe { openat::Dir::from_raw_fd(fd) }
        };
        Ok(dir)
    }

    /// Signals blobfs to unmount and waits for it to exit cleanly, returning the backing Ramdisk.
    pub async fn unmount(self) -> Result<Ramdisk, Error> {
        zx::Status::ok(self.proxy.unmount().await.context("sending blobfs unmount")?)
            .context("unmounting blobfs")?;

        self.process
            .wait_handle(
                zx::Signals::PROCESS_TERMINATED,
                zx::Time::after(zx::Duration::from_seconds(30)),
            )
            .context("waiting for 'blobfs mount' to exit")?;
        let ret = self.process.info().context("getting 'blobfs mount' process info")?.return_code;
        if ret != 0 {
            return Err(format_err!("'blobfs mount' returned nonzero exit code {}", ret));
        }

        Ok(self.backing_ramdisk)
    }

    /// Signals blobfs to unmount and waits for it to exit cleanly, stopping the inner ramdisk.
    pub async fn stop(self) -> Result<(), Error> {
        self.unmount().await?.stop()
    }

    /// Returns a sorted list of all blobs present in this blobfs instance.
    pub fn list_blobs(&self) -> Result<BTreeSet<Hash>, Error> {
        self.root_dir()?
            .list_dir(".")?
            .map(|entry| {
                Ok(entry?
                    .file_name()
                    .to_str()
                    .ok_or_else(|| anyhow::format_err!("expected valid utf-8"))?
                    .parse()?)
            })
            .collect()
    }

    /// Writes the blob to blobfs.
    pub fn add_blob_from(
        &self,
        merkle: &Hash,
        mut source: impl std::io::Read,
    ) -> Result<(), Error> {
        use std::{convert::TryInto, io::Write};

        let mut bytes = vec![];
        source.read_to_end(&mut bytes)?;
        let mut file = self.root_dir().unwrap().write_file(merkle.to_string(), 0777)?;
        file.set_len(bytes.len().try_into().unwrap())?;
        file.write_all(&bytes)?;
        Ok(())
    }
}

/// A virtual memory-backed block device.
pub struct Ramdisk {
    proxy: NodeProxy,
    client: RamdiskClient,
}

impl Ramdisk {
    /// Starts a new ramdisk with 1024 * 1024 blocks and a block size of 512 bytes, or a drive with
    /// 512MiB capacity.
    pub fn start() -> Result<Self, Error> {
        let client = RamdiskClient::create(512, 1 << 20)?;
        let proxy = NodeProxy::new(fuchsia_async::Channel::from_channel(client.open()?)?);
        Ok(Ramdisk { proxy, client })
    }

    fn clone_channel(&self) -> Result<zx::Channel, Error> {
        let (result, server_end) = zx::Channel::create()?;
        self.proxy.clone(fidl_fuchsia_io::CLONE_FLAG_SAME_RIGHTS, ServerEnd::new(server_end))?;
        Ok(result)
    }

    fn clone_handle(&self) -> Result<zx::Handle, Error> {
        Ok(self.clone_channel().context("cloning ramdisk channel")?.into())
    }

    /// Shuts down this ramdisk.
    pub fn stop(self) -> Result<(), Error> {
        Ok(self.client.destroy()?)
    }

    /// Corrupt the blob given by merkle, assuming this ramdisk is formatted as blobfs.
    pub async fn corrupt_blob(&self, merkle: &Hash) {
        let ramdisk = Clone::clone(&self.proxy);
        blobfs_corrupt_blob(ramdisk, merkle).await.unwrap();
    }
}

async fn blobfs_corrupt_blob(ramdisk: NodeProxy, merkle: &Hash) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.root_dir().add_service_at("block", |chan| {
        ramdisk
            .clone(
                fidl_fuchsia_io::CLONE_FLAG_SAME_RIGHTS | fidl_fuchsia_io::OPEN_FLAG_DESCRIBE,
                ServerEnd::new(chan),
            )
            .unwrap();
        None
    });

    let (devfs_client, devfs_server) = zx::Channel::create()?;
    fs.serve_connection(devfs_server)?;
    let serve_fs = fs.collect::<()>();

    let spawn_and_wait = async move {
        let p = fdio::spawn_etc(
            &fuchsia_runtime::job_default(),
            SpawnOptions::CLONE_ALL - SpawnOptions::CLONE_NAMESPACE,
            &CString::new("/pkg/bin/blobfs-corrupt").unwrap(),
            &[
                &CString::new("blobfs-corrupt").unwrap(),
                &CString::new("--device").unwrap(),
                &CString::new("/dev/block").unwrap(),
                &CString::new("--merkle").unwrap(),
                &CString::new(merkle.to_string()).unwrap(),
            ],
            None,
            &mut [SpawnAction::add_namespace_entry(
                &CString::new("/dev").unwrap(),
                devfs_client.into(),
            )],
        )
        .map_err(|(status, _)| status)
        .context("spawning 'blobfs-corrupt'")?;

        wait_for_process_async(p).await.context("'blobfs-corrupt'")?;
        Ok(())
    };

    let ((), res) = futures::join!(serve_fs, spawn_and_wait);

    res
}

async fn wait_for_process_async(proc: fuchsia_zircon::Process) -> Result<(), Error> {
    let signals =
        fuchsia_async::OnSignals::new(&proc.as_handle_ref(), zx::Signals::PROCESS_TERMINATED)
            .await
            .context("waiting for tool to terminate")?;
    assert_eq!(signals, zx::Signals::PROCESS_TERMINATED);

    let ret = proc.info().context("getting tool process info")?.return_code;
    if ret != 0 {
        return Err(format_err!("tool returned nonzero exit code {}", ret));
    }
    Ok(())
}

fn mkblobfs_block(block_device: zx::Handle) -> Result<(), Error> {
    let block_device_handle_id = HandleInfo::new(HandleType::User0, 1);
    let p = fdio::spawn_etc(
        &fuchsia_runtime::job_default(),
        SpawnOptions::CLONE_ALL,
        &CString::new("/pkg/bin/blobfs").unwrap(),
        &[&CString::new("blobfs").unwrap(), &CString::new("mkfs").unwrap()],
        None,
        &mut [SpawnAction::add_handle(block_device_handle_id, block_device)],
    )
    .map_err(|(status, _)| status)
    .context("spawning 'blobfs mkfs'")?;

    wait_for_process(p).context("'blobfs mkfs'")?;
    Ok(())
}

fn wait_for_process(proc: fuchsia_zircon::Process) -> Result<(), Error> {
    proc.wait_handle(
        zx::Signals::PROCESS_TERMINATED,
        zx::Time::after(zx::Duration::from_seconds(30)),
    )
    .context("waiting for tool to terminate")?;
    let ret = proc.info().context("getting tool process info")?.return_code;
    if ret != 0 {
        return Err(format_err!("tool returned nonzero exit code {}", ret));
    }
    Ok(())
}

fn mkblobfs(ramdisk: &Ramdisk) -> Result<(), Error> {
    mkblobfs_block(ramdisk.clone_handle()?)
}

/// An owned zircon job or process that is silently killed when dropped.
struct KillOnDrop<T: fuchsia_zircon::Task> {
    task: T,
}

impl<T: fuchsia_zircon::Task> Drop for KillOnDrop<T> {
    fn drop(&mut self) {
        let _ = self.task.kill();
    }
}

impl<T: fuchsia_zircon::Task> std::ops::Deref for KillOnDrop<T> {
    type Target = T;
    fn deref(&self) -> &Self::Target {
        &self.task
    }
}

impl<T: fuchsia_zircon::Task> From<T> for KillOnDrop<T> {
    fn from(task: T) -> Self {
        Self { task }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, std::io::Write};

    #[fuchsia_async::run_singlethreaded(test)]
    async fn clean_start_and_stop() {
        let blobfs = BlobfsRamdisk::start().unwrap();

        let proxy = blobfs.root_dir_proxy().unwrap();
        drop(proxy);

        blobfs.stop().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_appears_in_readdir() {
        let blobfs = BlobfsRamdisk::start().unwrap();
        let root = blobfs.root_dir().unwrap();

        let hello_merkle = write_blob(&root, "Hello blobfs!".as_bytes());
        assert_eq!(list_blobs(&root), vec![hello_merkle]);

        drop(root);
        blobfs.stop().await.unwrap();
    }

    /// Writes a blob to blobfs, returning the computed merkle root of the blob.
    fn write_blob(dir: &openat::Dir, payload: &[u8]) -> String {
        let merkle = fuchsia_merkle::MerkleTree::from_reader(payload).unwrap().root().to_string();

        let mut f = dir.new_file(&merkle, 0600).unwrap();
        f.set_len(payload.len() as u64).unwrap();
        f.write_all(payload).unwrap();

        merkle
    }

    /// Returns an unsorted list of blobs in the given blobfs dir.
    fn list_blobs(dir: &openat::Dir) -> Vec<String> {
        dir.list_dir(".")
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_owned().into_string().unwrap())
            .collect()
    }
}
