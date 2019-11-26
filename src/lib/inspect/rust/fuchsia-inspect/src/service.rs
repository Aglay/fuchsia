// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{reader::ReadableTree, Inspector},
    failure::{Error, ResultExt},
    fidl_fuchsia_inspect::{
        TreeContent, TreeNameIteratorRequest, TreeNameIteratorRequestStream, TreeRequest,
        TreeRequestStream, MAX_TREE_NAME_LIST_SIZE,
    },
    fidl_fuchsia_mem::Buffer,
    fuchsia_async as fasync,
    fuchsia_syslog::macros::*,
    futures::{TryFutureExt, TryStreamExt},
};

/// Runs a server for the `fuchsia.inspect.Tree` protocol. This protocol returns the VMO
/// associated with the given tree on `get_content` and allows to open linked trees (lazy nodes).
pub async fn handle_request_stream(
    inspector: Inspector,
    mut stream: TreeRequestStream,
) -> Result<(), Error> {
    while let Some(request) = stream.try_next().await.context("Error running tree server")? {
        match request {
            TreeRequest::GetContent { responder } => {
                let vmo = inspector.duplicate_vmo();
                let buffer_data = vmo.and_then(|vmo| vmo.get_size().ok().map(|size| (vmo, size)));
                let content = TreeContent {
                    buffer: buffer_data.map(|data| Buffer { vmo: data.0, size: data.1 }),
                };
                responder.send(content)?;
            }
            TreeRequest::ListChildNames { tree_iterator, .. } => {
                let values = inspector.tree_names().await?;
                let request_stream = tree_iterator.into_stream()?;
                spawn_tree_name_iterator_server(values, request_stream)
            }
            TreeRequest::OpenChild { child_name, tree, .. } => {
                if let Ok(inspector) = inspector.read_tree(&child_name).await {
                    spawn(inspector, tree.into_stream()?)
                }
            }
        }
    }
    Ok(())
}

/// Spawns a server for the `fuchsia.inspect.Tree` protocol. This protocol returns the VMO
/// associated with the given tree on `get_content` and allows to open linked trees (lazy nodes).
fn spawn(inspector: Inspector, stream: TreeRequestStream) {
    fasync::spawn(async move {
        handle_request_stream(inspector, stream)
            .await
            .unwrap_or_else(|e: Error| fx_log_err!("error running tree server: {:?}", e));
    });
}

/// Runs a server for the `fuchsia.inspect.TreeNameIterator` protocol. This protocol returns the
/// given list of values by chunks.
fn spawn_tree_name_iterator_server(values: Vec<String>, mut stream: TreeNameIteratorRequestStream) {
    fasync::spawn(
        async move {
            let mut values_iter = values.into_iter();
            while let Some(request) =
                stream.try_next().await.context("Error running tree iterator server")?
            {
                match request {
                    TreeNameIteratorRequest::GetNext { responder } => {
                        let result = values_iter
                            .by_ref()
                            .take(MAX_TREE_NAME_LIST_SIZE as usize)
                            .collect::<Vec<String>>();
                        if result.is_empty() {
                            responder.send(&mut vec![].into_iter())?;
                            return Ok(());
                        }
                        responder.send(&mut result.iter().map(|s| s.as_ref()))?;
                    }
                }
            }
            Ok(())
        }
        .unwrap_or_else(|e: Error| fx_log_err!("error running tree name iterator server: {:?}", e)),
    )
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            constants,
            reader::{NodeHierarchy, PartialNodeHierarchy},
            Inspector,
        },
        fidl_fuchsia_inspect::{
            TreeMarker, TreeNameIteratorMarker, TreeNameIteratorProxy, TreeProxy,
        },
        futures::FutureExt,
        std::convert::TryFrom,
    };

    #[fasync::run_singlethreaded(test)]
    async fn get_contents() -> Result<(), Error> {
        let tree = spawn_server()?;
        let tree_content = tree.get_content().await?;
        let hierarchy = parse_content(tree_content)?;
        assert_inspect_tree!(hierarchy, root: {
            a: 1i64,
        });
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn list_child_names() -> Result<(), Error> {
        let tree = spawn_server()?;
        let (name_iterator, server_end) =
            fidl::endpoints::create_proxy::<TreeNameIteratorMarker>()?;
        tree.list_child_names(server_end)?;
        verify_iterator(name_iterator, vec!["lazy-0".to_string()]).await?;
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn open_children() -> Result<(), Error> {
        let tree = spawn_server()?;
        let (child_tree, server_end) = fidl::endpoints::create_proxy::<TreeMarker>()?;
        tree.open_child("lazy-0", server_end)?;
        let tree_content = child_tree.get_content().await?;
        let hierarchy = parse_content(tree_content)?;
        assert_inspect_tree!(hierarchy, root: {
            b: 2u64,
        });
        let (name_iterator, server_end) =
            fidl::endpoints::create_proxy::<TreeNameIteratorMarker>()?;
        child_tree.list_child_names(server_end)?;
        verify_iterator(name_iterator, vec!["lazy-vals-0".to_string()]).await?;

        let (child_tree_2, server_end) = fidl::endpoints::create_proxy::<TreeMarker>()?;
        child_tree.open_child("lazy-vals-0", server_end)?;
        let tree_content = child_tree_2.get_content().await?;
        let hierarchy = parse_content(tree_content)?;
        assert_inspect_tree!(hierarchy, root: {
            c: 3.0,
        });
        let (name_iterator, server_end) =
            fidl::endpoints::create_proxy::<TreeNameIteratorMarker>()?;
        child_tree_2.list_child_names(server_end)?;
        verify_iterator(name_iterator, vec![]).await?;

        Ok(())
    }

    async fn verify_iterator(
        name_iterator: TreeNameIteratorProxy,
        values: Vec<String>,
    ) -> Result<(), Error> {
        if !values.is_empty() {
            assert_eq!(name_iterator.get_next().await?, values);
        }
        assert!(name_iterator.get_next().await?.is_empty());
        assert!(name_iterator.get_next().await.is_err());
        Ok(())
    }

    fn parse_content(tree_content: TreeContent) -> Result<NodeHierarchy, Error> {
        let buffer = tree_content.buffer.unwrap();
        assert_eq!(buffer.size, constants::DEFAULT_VMO_SIZE_BYTES as u64);
        Ok(PartialNodeHierarchy::try_from(&buffer.vmo)?.into())
    }

    fn spawn_server() -> Result<TreeProxy, Error> {
        let inspector = test_inspector();
        let (tree, request_stream) = fidl::endpoints::create_proxy_and_stream::<TreeMarker>()?;
        spawn(inspector, request_stream);
        Ok(tree)
    }

    fn test_inspector() -> Inspector {
        let inspector = Inspector::new();
        inspector.root().record_int("a", 1);
        inspector.root().record_lazy_child("lazy", || {
            async move {
                let inspector = Inspector::new();
                inspector.root().record_uint("b", 2);
                inspector.root().record_lazy_values("lazy-vals", || {
                    async move {
                        let inspector = Inspector::new();
                        inspector.root().record_double("c", 3.0);
                        Ok(inspector)
                    }
                    .boxed()
                });
                Ok(inspector)
            }
            .boxed()
        });
        inspector
    }
}
