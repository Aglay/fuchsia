// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod library_loader;

use {
    crate::data::DictionaryExt,
    crate::model::{Runner, RunnerError},
    crate::ns_util::{self, PKG_PATH},
    failure::{format_err, Error},
    fdio::fdio_sys,
    fidl_fuchsia_data as fdata, fidl_fuchsia_process as fproc, fidl_fuchsia_sys2 as fsys,
    fuchsia_app::client::connect_to_service,
    fuchsia_zircon::{self as zx, HandleBased},
    futures::future::FutureObj,
    std::path::PathBuf,
};

// TODO: the following should be sourced from //zircon/system/public/zircon/processargs.h
const PA_JOB_DEFAULT: u32 = 0x03;
const PA_LDSVC_LOADER: u32 = 0x10;

/// Runs components with ELF binaries.
pub struct ElfRunner {}

fn handles_from_fd(fd: i32) -> Result<Vec<fproc::HandleInfo>, Error> {
    // TODO(CF-592): fdio is not guaranteed to be asynchronous, replace with native rust solution
    unsafe {
        let mut fdio_handles = [zx::sys::ZX_HANDLE_INVALID; fdio_sys::FDIO_MAX_HANDLES as usize];
        let mut fdio_types = [0u32; fdio_sys::FDIO_MAX_HANDLES as usize];
        let handle_ptr = &mut fdio_handles[0] as *mut _ as *mut zx::sys::zx_handle_t;
        let type_ptr = &mut fdio_types[0] as *mut _ as *mut u32;
        let status = fdio_sys::fdio_clone_fd(fd, fd, handle_ptr, type_ptr);
        if status == zx::sys::ZX_ERR_BAD_HANDLE {
            // This file descriptor is closed. We just skip it rather than
            // generating an error.
            return Ok(vec![]);
        }
        if status < zx::sys::ZX_OK {
            return Err(format_err!("failed to clone fd {}: {}", fd, status));
        }
        let mut infos = vec![];
        for i in 0usize..(status as usize) {
            infos.push(fproc::HandleInfo {
                handle: zx::Handle::from_raw(fdio_handles[i]),
                id: fdio_types[i],
            });
        }
        Ok(infos)
    }
}

impl ElfRunner {
    pub fn new() -> ElfRunner {
        ElfRunner {}
    }

    // TODO: all internal error handling from here down uses failure::Error and converts into
    // RunnerError for returning purposes. This was because RunnerError was not descriptive enough
    // for debugging purposes. This has the unfortunate side effect of a smattering of
    // `.map_err(|e| eprintln!(...`'s scattered around. The RunnerError type should probably become
    // more expressive, or at least error handling in this function should be less tedious.
    async fn start_async(&self, start_info: fsys::ComponentStartInfo) -> Result<(), RunnerError> {
        // TODO: remove these unwraps
        let resolved_uri = PathBuf::from(get_resolved_uri(&start_info)?);
        let name = resolved_uri.file_name().unwrap().to_str().unwrap().clone();

        // Make a non-Option namespace
        let ns = start_info
            .ns
            .unwrap_or(fsys::ComponentNamespace { paths: vec![], directories: vec![] });

        // Load in a VMO holding the target executable from the namespace
        let (mut ns, ns_clone) = ns_util::clone_component_namespace(ns)
            .map_err(|_| RunnerError::ComponentNotAvailable)?;
        let ns_map =
            ns_util::ns_to_map(ns_clone).map_err(|_| RunnerError::ComponentNotAvailable)?;

        let bin_path: PathBuf = get_program_binary(&start_info.program)?;
        let executable_vmo =
            await!(library_loader::load_object(&ns_map, bin_path)).map_err(|e| {
                eprintln!("error loading object: {}", e);
                RunnerError::ComponentNotAvailable
            })?;

        // TODO: drop this unsafe block once the fuchsia_runtime crate exists.
        let default_job = zx::Job::from(unsafe { zx::Handle::from_raw(zx::sys::zx_job_default()) });

        let child_job =
            default_job.create_child_job().map_err(|_| RunnerError::ComponentNotAvailable)?; // TODO: handle this error better

        let child_job_dup = child_job.duplicate_handle(zx::Rights::SAME_RIGHTS).map_err(|e| {
            eprintln!("failed to duplicate handle to child job: {}", e);
            RunnerError::ComponentNotAvailable
        })?; // TODO: handle this error better

        let launcher = connect_to_service::<fproc::LauncherMarker>()
            .map_err(|_| RunnerError::ComponentNotAvailable)?;

        // Start the library loader service
        let (ll_client_chan, ll_service_chan) =
            zx::Channel::create().map_err(|_| RunnerError::ComponentNotAvailable)?;
        library_loader::start(ns_map, ll_service_chan);

        // TODO: launcher.AddArgs
        // TODO: launcher.AddEnvirons

        let mut handle_infos = vec![];
        for fd in 0..3 {
            handle_infos.append(&mut handles_from_fd(fd).map_err(|e| {
                eprintln!("error getting handles for {}: {}", fd, e);
                RunnerError::ComponentNotAvailable
            })?);
        }

        handle_infos.append(&mut vec![
            fproc::HandleInfo { handle: ll_client_chan.into_handle(), id: PA_LDSVC_LOADER },
            // TODO: is this needed?
            //fproc::HandleInfo{
            //    handle: ,
            //    id: PA_JOB_DEFAULT,
            //},
            // TODO: PA_DIRECTORY_REQUEST
        ]);
        launcher
            .add_handles(&mut handle_infos.iter_mut())
            .map_err(|_| RunnerError::ComponentNotAvailable)?;

        let mut name_infos = vec![];
        while let Some(path) = ns.paths.pop() {
            if let Some(directory) = ns.directories.pop() {
                name_infos.push(fproc::NameInfo { path, directory })
            }
        }
        launcher
            .add_names(&mut name_infos.iter_mut())
            .map_err(|_| RunnerError::ComponentNotAvailable)?;

        let (status, _process) = await!(launcher.launch(&mut fproc::LaunchInfo {
            executable: executable_vmo,
            job: child_job_dup,
            name: name.to_string(),
        }))
        .map_err(|e| {
            eprintln!("got an error from launcher.launch: {}", e);
            RunnerError::ComponentNotAvailable
        })?;
        let status = zx::Status::from_raw(status);
        if status != zx::Status::OK {
            eprintln!("failed to launch: {}", status,);
            return Err(RunnerError::ComponentNotAvailable);
        }
        Ok(())
    }
}

impl Runner for ElfRunner {
    fn start(&self, start_info: fsys::ComponentStartInfo) -> FutureObj<Result<(), RunnerError>> {
        FutureObj::new(Box::new(self.start_async(start_info)))
    }
}

fn get_resolved_uri(start_info: &fsys::ComponentStartInfo) -> Result<&str, RunnerError> {
    match &start_info.resolved_uri {
        Some(uri) => Ok(uri),
        _ => Err(RunnerError::InvalidArgs),
    }
}

fn get_program_binary(program: &Option<fdata::Dictionary>) -> Result<PathBuf, RunnerError> {
    if let Some(program) = program {
        if let Some(binary) = program.find("binary") {
            if let fdata::Value::Str(bin) = binary {
                return Ok(PKG_PATH.join(bin));
            }
        }
    }
    Err(RunnerError::InvalidArgs)
}

#[cfg(test)]
mod tests {
    use {
        crate::elf_runner::*, crate::io_util, fidl::endpoints::ClientEnd, fuchsia_async as fasync,
    };

    #[test]
    fn hello_world_test() {
        let mut executor = fasync::Executor::new().unwrap();
        executor.run_singlethreaded(
            async {
                // Get a handle to /bin
                let bin_path = "/pkg/bin".to_string();
                let bin_proxy = await!(io_util::open_directory_in_namespace("/pkg/bin")).unwrap();
                let bin_chan = bin_proxy.into_channel().unwrap();
                let bin_handle = ClientEnd::new(bin_chan.into_zx_channel());

                // Get a handle to /lib
                let lib_path = "/pkg/lib".to_string();
                let lib_proxy = await!(io_util::open_directory_in_namespace("/pkg/lib")).unwrap();
                let lib_chan = lib_proxy.into_channel().unwrap();
                let lib_handle = ClientEnd::new(lib_chan.into_zx_channel());

                let ns = fsys::ComponentNamespace {
                    paths: vec![lib_path, bin_path],
                    directories: vec![lib_handle, bin_handle],
                };

                let start_info = fsys::ComponentStartInfo {
                    resolved_uri: Some(
                        "fuchsia-pkg://fuchsia.com/hello_world_hippo#meta/hello_world.cm"
                            .to_string(),
                    ),
                    program: Some(fdata::Dictionary {
                        entries: vec![fdata::Entry {
                            key: "binary".to_string(),
                            value: Some(Box::new(fdata::Value::Str(
                                "/pkg/bin/hello_world".to_string(),
                            ))),
                        }],
                    }),
                    ns: Some(ns),
                };

                let runner = ElfRunner::new();
                await!(runner.start_async(start_info)).unwrap();
            },
        );
    }
}
