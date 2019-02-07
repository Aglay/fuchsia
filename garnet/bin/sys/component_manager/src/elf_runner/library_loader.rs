// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::elf_runner::io_util,
    crate::elf_runner::ns_util,
    failure::{err_msg, format_err, Error},
    fidl::endpoints::RequestStream,
    fidl_fuchsia_io::{DirectoryProxy, OPEN_RIGHT_READABLE},
    fidl_fuchsia_ldsvc::{LoaderRequest, LoaderRequestStream},
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::{TryFutureExt, TryStreamExt},
    std::collections::HashMap,
    std::path::{Path, PathBuf},
};

/// start will expose the ldsvc.fidl service over the given channel, providing VMO buffers of
/// requested library object names from the given namespace.
pub fn start(ns_map: HashMap<PathBuf, DirectoryProxy>, chan: zx::Channel) {
    fasync::spawn(
        async move {
            // Wait for requests
            let mut stream =
                LoaderRequestStream::from_channel(fasync::Channel::from_channel(chan)?);
            while let Some(req) = await!(stream.try_next())? {
                match req {
                    LoaderRequest::Done { control_handle } => {
                        control_handle.shutdown();
                    }
                    LoaderRequest::LoadObject { object_name, responder } => {
                        // The name provided by the client here has a null byte at the end, which
                        // doesn't work from here on out (io.fidl doesn't like it). Context:
                        // https://fuchsia-review.git.corp.google.com/c/zircon/+/121048/3/system/ulib/ldmsg/ldmsg.c#96
                        let object_name = object_name.trim_matches(char::from(0));
                        // TODO: the loader service in C will use the "lib" prefix for programs
                        // started with a root fd, and check under both "/system/lib" and
                        // "/boot/lib" for programs that don't (aka programs from the system
                        // package I think?). I don't know if we still need this behavior here.
                        let object_path = Path::new("/lib").join(object_name);
                        match await!(load_object(&ns_map, PathBuf::from(object_path))) {
                            Ok(b) => responder.send(zx::sys::ZX_OK, Some(b))?,
                            Err(e) => {
                                println!("failed to load object: {}", e);
                                responder.send(zx::sys::ZX_ERR_NOT_FOUND, None)?;
                            }
                        };
                    }
                    LoaderRequest::LoadScriptInterpreter { interpreter_name: _, responder } => {
                        // Unimplemented
                        responder.control_handle().shutdown();
                    }
                    LoaderRequest::Config { config: _, responder } => {
                        // Unimplemented
                        responder.control_handle().shutdown();
                    }
                    LoaderRequest::Clone { loader, responder } => {
                        let new_ns_map = ns_util::clone_component_namespace_map(&ns_map)?;
                        start(new_ns_map, loader.into_channel());
                        responder.send(zx::sys::ZX_OK)?;
                    }
                    LoaderRequest::DebugPublishDataSink { data_sink: _, data: _, responder } => {
                        // Unimplemented
                        responder.control_handle().shutdown();
                    }
                    LoaderRequest::DebugLoadConfig { config_name: _, responder } => {
                        // Unimplemented
                        responder.control_handle().shutdown();
                    }
                }
            }
            Ok(())
        }
            .unwrap_or_else(|e: Error| println!("couldn't run library loader service: {}", e)),
    ); // TODO
}

/// load_object will find the named object in the given namespace and return a VMO containing its
/// contents.
pub async fn load_object(
    ns_map: &HashMap<PathBuf, DirectoryProxy>,
    object_path: PathBuf,
) -> Result<zx::Vmo, Error> {
    for (ns_prefix, current_dir) in ns_map.iter() {
        if object_path.starts_with(ns_prefix) {
            let sub_path = pathbuf_drop_prefix(&object_path, ns_prefix);
            let file_proxy = await!(io_util::open_file(current_dir, &sub_path))?;
            let (status, fidlbuf) = await!(file_proxy.get_buffer(OPEN_RIGHT_READABLE))
                .map_err(|e| format_err!("get_buffer failed! {}", e))?;
            // TODO: check status
            return fidlbuf
                .map(|b| b.vmo)
                .ok_or(format_err!("bad status received on get_buffer: {}", status));
        }
    }
    Err(err_msg("requested library not found"))
}

fn pathbuf_drop_prefix(path: &PathBuf, prefix: &PathBuf) -> PathBuf {
    path.clone().iter().skip(prefix.iter().count()).collect()
}
