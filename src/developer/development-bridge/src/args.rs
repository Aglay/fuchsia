// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {crate::config::args::ConfigCommand, argh::FromArgs};

#[derive(FromArgs, Debug, PartialEq)]
/// Fuchsia Development Bridge
pub struct Ffx {
    #[argh(subcommand)]
    pub subcommand: Subcommand,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "daemon", description = "run as daemon")]
pub struct DaemonCommand {}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "echo", description = "run echo test")]
pub struct EchoCommand {
    #[argh(positional)]
    /// text string to echo back and forth
    pub text: Option<String>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "list", description = "list connected devices")]
pub struct ListCommand {
    #[argh(positional)]
    pub nodename: Option<String>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "run-component", description = "run component")]
pub struct RunComponentCommand {
    #[argh(positional)]
    /// url of component to run
    pub url: String,
    #[argh(positional)]
    /// args for the component
    pub args: Vec<String>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum Subcommand {
    Daemon(DaemonCommand),
    Echo(EchoCommand),
    List(ListCommand),
    RunComponent(RunComponentCommand),
    Config(ConfigCommand),
}

#[cfg(test)]
mod tests {
    use super::*;
    const CMD_NAME: &'static [&'static str] = &["ffx"];

    #[test]
    fn test_echo() {
        fn check(args: &[&str], expected_echo: &str) {
            assert_eq!(
                Ffx::from_args(CMD_NAME, args),
                Ok(Ffx {
                    subcommand: Subcommand::Echo(EchoCommand {
                        text: Some(expected_echo.to_string()),
                    })
                })
            )
        }

        let echo = "test-echo";

        check(&["echo", echo], echo);
    }

    #[test]
    fn test_daemon() {
        fn check(args: &[&str]) {
            assert_eq!(
                Ffx::from_args(CMD_NAME, args),
                Ok(Ffx { subcommand: Subcommand::Daemon(DaemonCommand {}) })
            )
        }

        check(&["daemon"]);
    }

    #[test]
    fn test_list() {
        fn check(args: &[&str], nodename: String) {
            assert_eq!(
                Ffx::from_args(CMD_NAME, args),
                Ok(Ffx { subcommand: Subcommand::List(ListCommand { nodename: Some(nodename) }) })
            )
        }

        let nodename = String::from("thumb-set-human-neon");
        check(&["list", &nodename], nodename.clone());
    }

    #[test]
    fn test_run_component() {
        fn check(args: &[&str], expected_url: String, expected_args: Vec<String>) {
            assert_eq!(
                Ffx::from_args(CMD_NAME, args),
                Ok(Ffx {
                    subcommand: Subcommand::RunComponent(RunComponentCommand {
                        url: expected_url,
                        args: expected_args,
                    })
                })
            )
        }

        let test_url = "http://test.com";
        let arg1 = "test1";
        let arg2 = "test2";
        let args = vec![arg1.to_string(), arg2.to_string()];

        check(&["run-component", test_url, arg1, arg2], test_url.to_string(), args);
    }
}
