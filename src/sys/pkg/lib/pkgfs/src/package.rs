// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Typesafe wrappers around an open package directory.

use {
    fidl::endpoints::{ClientEnd, Proxy, ServerEnd},
    fidl_fuchsia_io::{DirectoryMarker, DirectoryProxy, FileProxy},
    fuchsia_hash::Hash,
    thiserror::Error,
};

/// An error encountered while opening a package
#[derive(Debug, Error)]
#[allow(missing_docs)]
pub enum OpenError {
    #[error("the package does not exist")]
    NotFound,

    #[error("while opening the package: {0}")]
    Io(io_util::node::OpenError),
}

/// An open package directory
#[derive(Debug)]
pub struct Directory {
    proxy: DirectoryProxy,
}

impl Directory {
    pub(crate) fn new(proxy: DirectoryProxy) -> Self {
        Self { proxy }
    }

    /// Returns the current component's package directory.
    pub fn open_from_namespace() -> Result<Self, io_util::node::OpenError> {
        let dir =
            io_util::directory::open_in_namespace("/pkg", fidl_fuchsia_io::OPEN_RIGHT_READABLE)?;
        Ok(Self::new(dir))
    }

    /// Cleanly close the package directory, consuming self.
    pub async fn close(self) -> Result<(), io_util::node::CloseError> {
        io_util::directory::close(self.proxy).await
    }

    /// Ask pkgfs to also serve this package directory on the given directory request.
    pub fn reopen(
        &self,
        dir_request: ServerEnd<DirectoryMarker>,
    ) -> Result<(), io_util::node::CloneError> {
        io_util::directory::clone_onto_no_describe(&self.proxy, None, dir_request)
    }

    /// Reads the merkle root of the package.
    pub async fn merkle_root(&self) -> Result<Hash, anyhow::Error> {
        let f = self.open_file("meta", OpenRights::Read).await?;
        let merkle = io_util::file::read_to_string(&f).await?;
        Ok(merkle.parse()?)
    }

    /// Open the file in the package given by `path` with the given access `rights`.
    pub async fn open_file(
        &self,
        path: &str,
        rights: OpenRights,
    ) -> Result<FileProxy, io_util::node::OpenError> {
        io_util::directory::open_file(&self.proxy, path, rights.to_flags()).await
    }

    /// Unwraps the inner DirectoryProxy, consuming self.
    pub fn into_proxy(self) -> DirectoryProxy {
        self.proxy
    }

    /// Unwraps the inner fidl ClientEnd, consuming self.
    pub fn into_client_end(self) -> ClientEnd<DirectoryMarker> {
        self.proxy
            .into_channel()
            .expect("no other users of the wrapped channel")
            .into_zx_channel()
            .into()
    }
}

/// Possible open rights when opening a file within a package.
#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum OpenRights {
    Read,
    ReadExecute,
}

impl OpenRights {
    fn to_flags(&self) -> u32 {
        match self {
            OpenRights::Read => fidl_fuchsia_io::OPEN_RIGHT_READABLE,
            OpenRights::ReadExecute => {
                fidl_fuchsia_io::OPEN_RIGHT_READABLE | fidl_fuchsia_io::OPEN_RIGHT_EXECUTABLE
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, matches::assert_matches};

    #[fuchsia_async::run_singlethreaded(test)]
    async fn identity() {
        let pkg = Directory::open_from_namespace().unwrap();

        let (proxy, server_end) = fidl::endpoints::create_proxy().unwrap();
        assert_matches!(pkg.reopen(server_end), Ok(()));
        assert_matches!(Directory::new(proxy).close().await, Ok(()));

        pkg.into_client_end();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn merkle_root() {
        let pkg = Directory::open_from_namespace().unwrap();

        let merkle: Hash = std::fs::read_to_string("/pkg/meta").unwrap().parse().unwrap();

        assert_eq!(pkg.merkle_root().await.unwrap(), merkle);
    }
}
