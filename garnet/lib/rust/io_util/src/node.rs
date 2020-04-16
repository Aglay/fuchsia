// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Utility functions for fuchsia.io nodes.

use {
    fidl_fuchsia_io::{
        DirectoryEvent, DirectoryObject, DirectoryProxy, FileEvent, FileObject, FileProxy,
        NodeInfo, NodeMarker, NodeProxy,
    },
    fuchsia_zircon::{self as zx, Status},
    futures::prelude::*,
    thiserror::Error,
};

/// An error encountered while opening a node
#[derive(Debug, Error)]
#[allow(missing_docs)]
pub enum OpenError {
    #[error("while making a fidl proxy: {}", _0)]
    CreateProxy(#[source] fidl::Error),

    #[error("while opening from namespace: {}", _0)]
    Namespace(Status),

    #[error("while sending open request: {}", _0)]
    SendOpenRequest(#[source] fidl::Error),

    #[error("node event stream closed prematurely")]
    OnOpenEventStreamClosed,

    #[error("while reading OnOpen event: {}", _0)]
    OnOpenDecode(#[source] fidl::Error),

    #[error("open failed with status: {}", _0)]
    OpenError(Status),

    #[error("remote responded with success but provided no node info")]
    MissingOnOpenInfo,

    #[error("expected node to be a {:?}, but got a {:?}", expected, actual)]
    UnexpectedNodeKind { expected: Kind, actual: Kind },
}

/// An error encountered while cloning a node
#[derive(Debug, Error)]
#[allow(missing_docs)]
pub enum CloneError {
    #[error("while making a fidl proxy: {}", _0)]
    CreateProxy(#[source] fidl::Error),

    #[error("while sending clone request: {}", _0)]
    SendCloneRequest(#[source] fidl::Error),
}

/// An error encountered while closing a node
#[derive(Debug, Error)]
#[allow(missing_docs)]
pub enum CloseError {
    #[error("while sending close request: {}", _0)]
    SendCloseRequest(fidl::Error),

    #[error("close failed with status: {}", _0)]
    CloseError(Status),
}

/// The type of a filesystem node
#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum Kind {
    Service,
    File,
    Directory,
    Pipe,
    Vmofile,
    Device,
    Tty,
    DatagramSocket,
    StreamSocket,
}

impl Kind {
    fn kind_of(info: &NodeInfo) -> Kind {
        match info {
            NodeInfo::Service(_) => Kind::Service,
            NodeInfo::File(_) => Kind::File,
            NodeInfo::Directory(_) => Kind::Directory,
            NodeInfo::Pipe(_) => Kind::Pipe,
            NodeInfo::Vmofile(_) => Kind::Vmofile,
            NodeInfo::Device(_) => Kind::Device,
            NodeInfo::Tty(_) => Kind::Tty,
            NodeInfo::DatagramSocket(_) => Kind::DatagramSocket,
            NodeInfo::StreamSocket(_) => Kind::StreamSocket,
        }
    }

    fn expect_file(info: NodeInfo) -> Result<Option<zx::Event>, Kind> {
        match info {
            NodeInfo::File(FileObject { event, stream: None }) => Ok(event),
            other => Err(Kind::kind_of(&other)),
        }
    }

    fn expect_directory(info: NodeInfo) -> Result<(), Kind> {
        match info {
            NodeInfo::Directory(DirectoryObject) => Ok(()),
            other => Err(Kind::kind_of(&other)),
        }
    }
}

// TODO namespace.connect is synchronous and may involve fdio making synchronous fidl calls to
// remote directories.  If/when fdio exposes the root namespace mapping or an API to connect to
// nodes asynchronously, this function should be updated to use that.
/// Connect a zx::Channel to a path in the current namespace.
pub fn connect_in_namespace(path: &str, flags: u32, chan: zx::Channel) -> Result<(), zx::Status> {
    let namespace = fdio::Namespace::installed()?;
    namespace.connect(path, flags, chan)?;
    Ok(())
}

/// Opens the given `path` from the current namespace as a [`NodeProxy`]. The target is not
/// verified to be any particular type and may not implement the fuchsia.io.Node protocol.
pub fn open_in_namespace(path: &str, flags: u32) -> Result<NodeProxy, OpenError> {
    let (node, server_end) =
        fidl::endpoints::create_proxy::<NodeMarker>().map_err(OpenError::CreateProxy)?;

    connect_in_namespace(path, flags, server_end.into_channel()).map_err(OpenError::Namespace)?;

    Ok(node)
}

/// Gracefully closes the node proxy from the remote end.
pub async fn close(node: NodeProxy) -> Result<(), CloseError> {
    let status = node.close().await.map_err(CloseError::SendCloseRequest)?;
    Status::ok(status).map_err(CloseError::CloseError)
}

/// Consume the first event from this DirectoryProxy's event stream, returning the proxy if it is
/// the expected type or an error otherwise.
pub(crate) async fn verify_directory_describe_event(
    node: DirectoryProxy,
) -> Result<DirectoryProxy, OpenError> {
    let mut events = node.take_event_stream();
    let DirectoryEvent::OnOpen_ { s: status, info } = events
        .next()
        .await
        .ok_or(OpenError::OnOpenEventStreamClosed)?
        .map_err(OpenError::OnOpenDecode)?;

    let () = Status::ok(status).map_err(OpenError::OpenError)?;

    let info = info.ok_or(OpenError::MissingOnOpenInfo)?;

    let () = Kind::expect_directory(*info)
        .map_err(|actual| OpenError::UnexpectedNodeKind { expected: Kind::Directory, actual })?;

    Ok(node)
}

/// Consume the first event from this FileProxy's event stream, returning the proxy if it is the
/// expected type or an error otherwise.
pub(crate) async fn verify_file_describe_event(node: FileProxy) -> Result<FileProxy, OpenError> {
    let mut events = node.take_event_stream();
    let FileEvent::OnOpen_ { s: status, info } = events
        .next()
        .await
        .ok_or(OpenError::OnOpenEventStreamClosed)?
        .map_err(OpenError::OnOpenDecode)?;

    let () = Status::ok(status).map_err(OpenError::OpenError)?;

    let info = info.ok_or(OpenError::MissingOnOpenInfo)?;

    let _event = Kind::expect_file(*info)
        .map_err(|actual| OpenError::UnexpectedNodeKind { expected: Kind::File, actual })?;

    Ok(node)
}

#[cfg(test)]
mod tests {
    use {super::*, crate::OPEN_RIGHT_READABLE, fuchsia_async as fasync, matches::assert_matches};

    // open_in_namespace

    #[fasync::run_singlethreaded(test)]
    async fn open_in_namespace_opens_real_node() {
        let file_node = open_in_namespace("/pkg/data/file", OPEN_RIGHT_READABLE).unwrap();
        let info = file_node.describe().await.unwrap();
        assert_matches!(Kind::expect_file(info), Ok(Some(_)));

        let dir_node = open_in_namespace("/pkg/data", OPEN_RIGHT_READABLE).unwrap();
        let info = dir_node.describe().await.unwrap();
        assert_eq!(Kind::expect_directory(info), Ok(()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn open_in_namespace_opens_fake_node_under_of_root_namespace_entry() {
        let notfound = open_in_namespace("/pkg/fake", OPEN_RIGHT_READABLE).unwrap();
        // The open error is not detected until the proxy is interacted with.
        assert_matches!(close(notfound).await, Err(_));
    }

    #[fasync::run_singlethreaded(test)]
    async fn open_in_namespace_rejects_fake_root_namespace_entry() {
        assert_matches!(
            open_in_namespace("/fake", OPEN_RIGHT_READABLE),
            Err(OpenError::Namespace(Status::NOT_FOUND))
        );
    }
}
