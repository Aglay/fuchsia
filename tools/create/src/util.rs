// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::anyhow;
use std::ffi::OsStr;
use std::path::PathBuf;
use std::{env, fs, io};

/// Return the fuchsia root directory.
pub fn get_fuchsia_root() -> Result<PathBuf, anyhow::Error> {
    let dir = env::var("FUCHSIA_DIR")?;
    Ok(fs::canonicalize(dir)?)
}

/// Returns the path to the template files.
pub fn get_templates_dir_path() -> io::Result<PathBuf> {
    let exe_path = env::current_exe()?;
    let exe_dir_path = exe_path.parent().ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidData, anyhow!("exe directory is root"))
    })?;
    Ok(exe_dir_path.join("create_templates"))
}

/// Converts an OS-specific filename to a String, returning an io::Error
/// if a failure occurs. The io::Error contains the invalid filename,
/// which gives the user a better indication of what went wrong.
pub fn filename_to_string(filename: impl AsRef<OsStr>) -> io::Result<String> {
    let filename = filename.as_ref();
    Ok(filename
        .to_str()
        .ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, anyhow!("invalid filename {:?}", filename))
        })?
        .to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::env;
    use std::os::unix;
    use tempfile::tempdir;

    #[test]
    fn get_fuchsia_root_resolves_symlinks() {
        let temp_dir = tempdir().expect("internal error: failed to create temp dir");
        let subdir_path = temp_dir.path().join("subdir");
        let link_path = temp_dir.path().join("symlink");
        let _subdir =
            std::fs::File::create(&subdir_path).expect("internal error: failed to create sub dir");
        let _link = unix::fs::symlink(&subdir_path, &link_path);
        env::set_var("FUCHSIA_DIR", link_path);

        let fuchsia_root = get_fuchsia_root();
        assert!(fuchsia_root.is_ok(), fuchsia_root);
        assert_eq!(fuchsia_root.unwrap(), subdir_path);
    }
}
