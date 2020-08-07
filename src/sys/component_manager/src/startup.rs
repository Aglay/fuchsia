// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};

/// Command line arguments that control component_manager's behavior. Use [Arguments::from_args()]
/// or [Arguments::new()] to create an instance.
// structopt would be nice to use here but the binary size impact from clap - which it depends on -
// is too large.
#[derive(Debug, Default, Clone, Eq, PartialEq)]
pub struct Arguments {
    /// If true, component_manager will serve an instance of fuchsia.process.Launcher and use this
    /// launcher for the built-in ELF component runner. The root component can additionally
    /// use and/or offer this service using '/builtin/fuchsia.process.Launcher' from realm.
    // This argument exists because the built-in process launcher *only* works when
    // component_manager runs under a job that has ZX_POL_NEW_PROCESS set to allow, like the root
    // job. Otherwise, the component_manager process cannot directly create process through
    // zx_process_create. When we run component_manager elsewhere, like in test environments, it
    // has to use the fuchsia.process.Launcher service provided through its namespace instead.
    pub use_builtin_process_launcher: bool,

    /// If true, component_manager will maintain a UTC kernel clock and vend write handles through
    /// an instance of `fuchsia.time.Maintenance`. This flag should only be used with the top-level
    /// component_manager.
    pub maintain_utc_clock: bool,

    /// URL of the root component to launch.
    pub root_component_url: String,

    /// If set, load component_manager's configuration from this path.
    /// This parameter is deprecated, and will be merged with --config.
    pub runtime_config: Option<String>,

    /// Load component_manager's configuration from this path.
    pub config: String,
}

impl Arguments {
    /// Parse `Arguments` from the given String Iterator.
    ///
    /// This parser is relatively simple since component_manager is not a user-facing binary that
    /// requires or would benefit from more flexible UX. Recognized arguments are extracted from
    /// the given Iterator and used to create the returned struct. Unrecognized flags starting with
    /// "--" result in an error being returned. A single non-flag argument is expected for the root
    /// component URL.
    pub fn new<I>(iter: I) -> Result<Self, Error>
    where
        I: IntoIterator<Item = String>,
    {
        let mut iter = iter.into_iter();
        let mut args = Self::default();
        while let Some(arg) = iter.next() {
            if arg == "--use-builtin-process-launcher" {
                args.use_builtin_process_launcher = true;
            } else if arg == "--maintain-utc-clock" {
                args.maintain_utc_clock = true;
            } else if arg == "--runtime-config" {
                args.runtime_config = iter.next();
                if args.runtime_config.is_none() {
                    return Err(format_err!("No value given for '--runtime_config'"));
                }
            } else if arg == "--config" {
                args.config = match iter.next() {
                    Some(config) => config,
                    None => return Err(format_err!("No value given for '--config'")),
                }
            } else if arg.starts_with("--") {
                return Err(format_err!("Unrecognized flag: {}", arg));
            } else {
                if !args.root_component_url.is_empty() {
                    return Err(format_err!("Multiple non-flag arguments given"));
                }
                args.root_component_url = arg;
            }
        }

        if args.root_component_url.is_empty() {
            return Err(format_err!("No root component URL found"));
        }
        if args.config.is_empty() {
            return Err(format_err!("No config file path found"));
        }
        Ok(args)
    }

    /// Parse `Arguments` from [std::env::args()].
    ///
    /// See [Arguments::new()] for more details.
    pub fn from_args() -> Result<Self, Error> {
        // Ignore first argument with executable name, then delegate to generic iterator impl.
        Self::new(std::env::args().skip(1))
    }

    /// Returns a usage message for the supported arguments.
    pub fn usage() -> String {
        format!(
            "Usage: {} [options] --config <path-to-config> <root-component-url>\n\
             Options:\n\
             --use-builtin-process-launcher   Provide and use a built-in implementation of\n\
             fuchsia.process.Launcher\n
             --maintain-utc-clock             Create and vend a UTC kernel clock through a\n\
             built-in implementation of fuchsia.time.Maintenance.\n\
             Should only be used with the root component_manager.\n",
            std::env::args().next().unwrap_or("component_manager".to_string())
        )
    }
}

#[cfg(test)]
mod tests {
    use {super::*, fuchsia_async as fasync};

    #[fasync::run_singlethreaded(test)]
    async fn parse_arguments() -> Result<(), Error> {
        let config_filename = || "foo".to_string();
        let config = || "--config".to_string();
        let dummy_url = || "fuchsia-pkg://fuchsia.com/pkg#meta/component.cm".to_string();
        let dummy_url2 = || "fuchsia-pkg://fuchsia.com/pkg#meta/component2.cm".to_string();
        let unknown_flag = || "--unknown".to_string();
        let use_builtin_launcher = || "--use-builtin-process-launcher".to_string();
        let maintain_utc_clock = || "--maintain-utc-clock".to_string();

        // Zero or multiple positional arguments is an error; must be exactly one URL.
        assert!(Arguments::new(vec![]).is_err());
        assert!(Arguments::new(vec![use_builtin_launcher()]).is_err());
        assert!(Arguments::new(vec![dummy_url(), dummy_url2()]).is_err());
        assert!(Arguments::new(vec![dummy_url(), use_builtin_launcher(), dummy_url2()]).is_err());

        // An unknown option is an error.
        assert!(Arguments::new(vec![unknown_flag()]).is_err());
        assert!(Arguments::new(vec![unknown_flag(), dummy_url()]).is_err());
        assert!(Arguments::new(vec![dummy_url(), unknown_flag()]).is_err());

        // Single positional argument with no options is parsed correctly
        assert_eq!(
            Arguments::new(vec![config(), config_filename(), dummy_url()])
                .expect("Unexpected error with just URL"),
            Arguments {
                config: config_filename(),
                root_component_url: dummy_url(),
                ..Default::default()
            }
        );
        assert_eq!(
            Arguments::new(vec![config(), config_filename(), dummy_url2()])
                .expect("Unexpected error with just URL"),
            Arguments {
                config: config_filename(),
                root_component_url: dummy_url2(),
                ..Default::default()
            }
        );

        // Options are parsed correctly and do not depend on order.
        assert_eq!(
            Arguments::new(vec![config(), config_filename(), use_builtin_launcher(), dummy_url()])
                .expect("Unexpected error with option"),
            Arguments {
                config: config_filename(),
                use_builtin_process_launcher: true,
                root_component_url: dummy_url(),
                ..Default::default()
            }
        );
        assert_eq!(
            Arguments::new(vec![config(), config_filename(), dummy_url(), use_builtin_launcher()])
                .expect("Unexpected error with option"),
            Arguments {
                config: config_filename(),
                use_builtin_process_launcher: true,
                root_component_url: dummy_url(),
                ..Default::default()
            }
        );
        assert_eq!(
            Arguments::new(vec![config(), config_filename(), dummy_url(), use_builtin_launcher(),])
                .expect("Unexpected error with option"),
            Arguments {
                config: config_filename(),
                use_builtin_process_launcher: true,
                root_component_url: dummy_url(),
                ..Default::default()
            }
        );
        assert_eq!(
            Arguments::new(vec![
                config(),
                config_filename(),
                dummy_url(),
                use_builtin_launcher(),
                maintain_utc_clock()
            ])
            .expect("Unexpected error with option"),
            Arguments {
                config: config_filename(),
                use_builtin_process_launcher: true,
                root_component_url: dummy_url(),
                maintain_utc_clock: true,
                ..Default::default()
            }
        );

        Ok(())
    }
}
