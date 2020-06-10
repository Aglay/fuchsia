// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use proc_macro_hack::proc_macro_hack;

#[proc_macro_hack]
pub use config_proc_macros::include_default;

#[macro_export]
macro_rules! ffx_cmd {
    () => {{
        #[cfg(test)]
        {
            ffx_lib_args::Ffx {
                config: None,
                subcommand: ffx_lib_sub_command::Subcommand::Daemon(
                    ffx_core::args::DaemonCommand {},
                ),
            }
        }
        #[cfg(not(test))]
        {
            argh::from_env()
        }
    }};
}

#[macro_export]
macro_rules! ffx_env {
    () => {{
        #[cfg(test)]
        {
            // Prevent any File I/O in unit tests.
            Err(anyhow::anyhow!("test - no environment"))
        }
        #[cfg(not(test))]
        {
            ffx_config::find_env_file()
        }
    }};
}

#[macro_export]
macro_rules! get {
    (str, $key:expr, $default:expr) => {{
        ffx_config::get_config_str($key, $default, ffx_config::ffx_cmd!(), ffx_config::ffx_env!())
    }};
    (bool, $key:expr, $default:expr) => {{
        ffx_config::get_config_bool($key, $default, ffx_config::ffx_cmd!(), ffx_config::ffx_env!())
    }};
    ($key:expr) => {
        ffx_config::get_config($key, ffx_config::ffx_cmd!(), ffx_config::ffx_env!())
    };
}

#[macro_export]
macro_rules! remove {
    ($level:expr, $key:expr, $build_dir:expr) => {{
        ffx_config::remove_config_with_build_dir(
            $level,
            $key,
            $build_dir,
            ffx_config::ffx_cmd!(),
            ffx_config::ffx_env!(),
        )
    }};
}

#[macro_export]
macro_rules! set {
    ($level:expr, $key:expr, $value:expr, $build_dir:expr) => {{
        ffx_config::set_config_with_build_dir(
            $level,
            $key,
            $value,
            $build_dir,
            ffx_config::ffx_cmd!(),
            ffx_config::ffx_env!(),
        )
    }};
}
