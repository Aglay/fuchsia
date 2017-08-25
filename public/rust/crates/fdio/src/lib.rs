// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Bindings for the Magenta mxio library

extern crate magenta;
extern crate magenta_sys;

mod mxio_sys;

use magenta_sys as sys;

use std::ffi::CStr;
use std::fs::File;
use std::os::raw;
use std::ffi;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::AsRawFd;
use std::path::Path;

pub use mxio_sys::mxio_ioctl as ioctl;

/// Events that can occur while watching a directory, including files that already exist prior to
/// running a Watcher.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum WatchEvent {
    /// A file was added.
    AddFile,

    /// A file was removed.
    RemoveFile,

    /// The Watcher has enumerated all the existing files and has started to wait for new files to
    /// be added.
    Idle,

    Unknown(i32),

    #[doc(hidden)]
    #[allow(non_camel_case_types)]
    // Try to prevent exhaustive matching since this enum may grow if mxio's events expand.
    __do_not_match,
}

impl From<raw::c_int> for WatchEvent {
    fn from(i: raw::c_int) -> WatchEvent {
        match i {
            mxio_sys::WATCH_EVENT_ADD_FILE => WatchEvent::AddFile,
            mxio_sys::WATCH_EVENT_REMOVE_FILE => WatchEvent::RemoveFile,
            mxio_sys::WATCH_EVENT_IDLE => WatchEvent::Idle,
            _ => WatchEvent::Unknown(i),
        }
    }
}

impl From<WatchEvent> for raw::c_int {
    fn from(i: WatchEvent) -> raw::c_int {
        match i {
            WatchEvent::AddFile => mxio_sys::WATCH_EVENT_ADD_FILE,
            WatchEvent::RemoveFile => mxio_sys::WATCH_EVENT_REMOVE_FILE,
            WatchEvent::Idle => mxio_sys::WATCH_EVENT_IDLE,
            WatchEvent::Unknown(i) => i as raw::c_int,
            _ => -1 as raw::c_int,
        }
    }
}

unsafe extern "C" fn watcher_cb<F>(
    _dirfd: raw::c_int,
    event: raw::c_int,
    fn_: *const raw::c_char,
    watcher: *const raw::c_void,
) -> sys::mx_status_t
where
    F: Sized + FnMut(WatchEvent, &Path) -> Result<(), magenta::Status>,
{
    let cb: &mut F = &mut *(watcher as *mut F);
    let filename = ffi::OsStr::from_bytes(CStr::from_ptr(fn_).to_bytes());
    match cb(WatchEvent::from(event), Path::new(filename)) {
        Ok(()) => sys::MX_OK,
        Err(e) => e as i32,
    }
}

/// Runs the given callback for each file in the directory and each time a new file is
/// added to the directory.
///
/// If the callback returns an error, the watching stops, and the magenta::Status is returned.
///
/// This function blocks for the duration of the watch operation. The deadline parameter will stop
/// the watch at the given (absolute) time and return magenta::Status::ErrTimedOut. A deadline of
/// magenta::MX_TIME_INFINITE will never expire.
///
/// The callback may use magenta::ErrStop as a way to signal to the caller that it wants to
/// stop because it found what it was looking for. Since this error code is not returned by
/// syscalls or public APIs, the callback does not need to worry about it turning up normally.
pub fn watch_directory<F>(dir: &File, deadline: sys::mx_time_t, mut f: F) -> magenta::Status
where
    F: Sized + FnMut(WatchEvent, &Path) -> Result<(), magenta::Status>,
{
    let cb_ptr: *mut raw::c_void = &mut f as *mut _ as *mut raw::c_void;
    unsafe {
        magenta::Status::from_raw(mxio_sys::mxio_watch_directory(
            dir.as_raw_fd(),
            watcher_cb::<F>,
            deadline,
            cb_ptr,
        ))
    }
}

/// Calculates an IOCTL value from kind, family and number.
pub fn make_ioctl(kind: i32, family: i32, number: i32) -> i32 {
    ((((kind) & 0xF) << 20) | (((family) & 0xFF) << 8) | ((number) & 0xFF))
}

pub const IOCTL_KIND_DEFAULT: i32 = 0;
pub const IOCTL_KIND_GET_HANDLE: i32 = 0x1;

pub const IOCTL_FAMILY_DEVICE: i32 = 0x01;
pub const IOCTL_FAMILY_CONSOLE: i32 = 0x10;
pub const IOCTL_FAMILY_INPUT: i32 = 0x11;
pub const IOCTL_FAMILY_DISPLAY: i32 = 0x12;
