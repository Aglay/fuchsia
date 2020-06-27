// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::target::TargetAddr,
    anyhow::{anyhow, Error},
    ffx_config::get,
    ffx_core::constants::{SSH_PORT, SSH_PRIV},
    std::collections::HashSet,
    std::process::Command,
};

static DEFAULT_SSH_OPTIONS: &'static [&str] = &[
    "-o CheckHostIP=no",
    "-o StrictHostKeyChecking=no",
    "-o UserKnownHostsFile=/dev/null",
    "-o ServerAliveInterval=1",
    "-o ServerAliveCountMax=10",
    "-o LogLevel=ERROR",
];

pub async fn build_ssh_command(
    addrs: HashSet<TargetAddr>,
    command: Vec<&str>,
) -> Result<Command, Error> {
    if command.is_empty() {
        return Err(anyhow!("missing SSH command"));
    }

    let port = get!(str, SSH_PORT, "").await;
    let key = get!(str, SSH_PRIV, "").await;

    let mut c = Command::new("ssh");

    if port.len() > 0 {
        c.arg("-p").arg(port);
    }

    if key.len() > 0 {
        c.arg("-i").arg(key);
    }

    let addr = addrs.iter().next().ok_or(anyhow!("no IP's for chosen target"))?;

    c.args(DEFAULT_SSH_OPTIONS).arg(format!("{}", addr)).args(&command);
    return Ok(c);
}

#[cfg(test)]
mod test {
    use {
        super::build_ssh_command,
        crate::target::TargetAddr,
        std::collections::HashSet,
        std::net::{IpAddr, Ipv4Addr},
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_empty_command_vec_produces_error() {
        let result = build_ssh_command(HashSet::new(), vec![]).await;
        assert!(result.is_err(), "empty command vec should produce an error");
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_no_ips_produces_error() {
        let result = build_ssh_command(HashSet::new(), vec!["ls"]).await;
        assert!(result.is_err(), "target with no IP's should produce an error");
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_valid_inputs() {
        let key_path = std::env::current_exe().unwrap();
        let key_path = key_path.to_str().take().unwrap();
        std::env::set_var("FUCHSIA_SSH_PORT", "1234");
        std::env::set_var("FUCHSIA_SSH_KEY", key_path);

        let ip = IpAddr::V4(Ipv4Addr::new(192, 168, 0, 1));
        let mut addrs = HashSet::new();
        addrs.insert(TargetAddr::from((ip, 0)));

        let result = build_ssh_command(addrs, vec!["ls"]).await.unwrap();
        let dbgstr = format!("{:?}", result);

        assert!(dbgstr.contains(&format!("\"-p\" \"1234\" \"-i\" \"{}\"", key_path)), dbgstr);
        assert!(dbgstr.contains(&ip.to_string()), dbgstr);
    }
}
