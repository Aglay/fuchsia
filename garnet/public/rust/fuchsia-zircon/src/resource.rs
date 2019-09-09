// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Type-safe bindings for Zircon resources.

use crate::ok;
use crate::{AsHandleRef, Handle, HandleBased, HandleRef, Status};
use fuchsia_zircon_sys as sys;

/// An object representing a Zircon resource.
///
/// As essentially a subtype of `Handle`, it can be freely interconverted.
#[derive(Debug, Eq, PartialEq)]
#[repr(transparent)]
pub struct Resource(Handle);
impl_handle_based!(Resource);

impl Resource {
    /// Create a child resource object.
    ///
    /// Wraps the
    /// [zx_resource_create](https://fuchsia.googlesource.com/fuchsia/+/master/docs/zircon/syscalls/resource_create.md)
    /// syscall
    pub fn create_child(
        &self,
        options: u32,
        base: u64,
        size: usize,
        name: &[u8],
    ) -> Result<Resource, Status> {
        let mut resource_out = 0;
        let name_ptr = name.as_ptr();
        let name_len = name.len();
        let status = unsafe {
            sys::zx_resource_create(
                self.raw_handle(),
                options,
                base,
                size,
                name_ptr,
                name_len,
                &mut resource_out,
            )
        };
        ok(status)?;
        unsafe { Ok(Resource::from(Handle::from_raw(resource_out))) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn create_child() {
        let invalid_resource = Resource::from(Handle::invalid());
        assert_eq!(
            invalid_resource.create_child(sys::ZX_RSRC_KIND_VMEX, 0, 0, b"vmex"),
            Err(Status::BAD_HANDLE)
        );
    }
}
