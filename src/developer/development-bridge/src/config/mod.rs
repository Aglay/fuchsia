// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::config::api::{PersistentConfig, ReadConfig},
    crate::config::config::Config,
    crate::config::environment::Environment,
    crate::config::heuristic_config::HeuristicFn,
    crate::constants::{ENV_FILE, LOG_DIR, LOG_ENABLED, SSH_PRIV, SSH_PUB},
    anyhow::{anyhow, Error},
    serde_json::Value,
    std::{
        collections::HashMap,
        env,
        fs::File,
        io::Write,
        path::{Path, PathBuf},
    },
};

mod api;
pub mod args;
pub mod command;
mod config;
mod env_var_config;
mod environment;
mod file_backed_config;
mod heuristic_config;
mod persistent_config;
mod priority_config;
mod runtime_config;

pub fn get_config(name: &str) -> Result<Option<Value>, Error> {
    get_config_with_build_dir(name, &None)
}

pub fn get_config_with_build_dir(
    name: &str,
    build_dir: &Option<String>,
) -> Result<Option<Value>, Error> {
    let config = load_config(build_dir)?;
    Ok(config.get(name))
}

pub fn get_config_str(name: &str, default: &str) -> String {
    get_config(name)
        .unwrap_or(Some(Value::String(default.to_string())))
        .map_or(default.to_string(), |v| v.as_str().unwrap_or(default).to_string())
}

pub fn get_config_bool(name: &str, default: bool) -> bool {
    get_config(name).unwrap_or(Some(Value::Bool(default))).map_or(default, |v| {
        v.as_bool().unwrap_or(match v {
            Value::String(s) => s.parse().unwrap_or(default),
            _ => default,
        })
    })
}

// TODO(fxr/45489): replace with the dirs::config_dir when the crate is included in third_party
// https://docs.rs/dirs/1.0.5/dirs/fn.config_dir.html
fn find_env_dir() -> Result<String, Error> {
    match env::var("HOME").or_else(|_| env::var("HOMEPATH")) {
        Ok(dir) => Ok(dir),
        Err(e) => Err(anyhow!("Could not determing environment directory: {}", e)),
    }
}

fn init_env_file(path: &PathBuf) -> Result<(), Error> {
    let mut f = File::create(path)?;
    f.write_all(b"{}")?;
    f.sync_all()?;
    Ok(())
}

pub(crate) fn find_env_file() -> Result<String, Error> {
    let mut env_path = PathBuf::from(find_env_dir()?);
    env_path.push(ENV_FILE);

    if !env_path.is_file() {
        log::debug!("initializing environment {}", env_path.display());
        init_env_file(&env_path)?;
    }
    match env_path.to_str() {
        Some(f) => Ok(String::from(f)),
        None => Err(anyhow!("Could not find environment file")),
    }
}

pub fn save_config(config: &mut Config<'_>, build_dir: Option<String>) -> Result<(), Error> {
    let file = find_env_file()?;
    let env = Environment::load(&file)?;

    match build_dir {
        Some(b) => config.save(&env.global, &env.build.as_ref().and_then(|c| c.get(&b)), &env.user),
        None => config.save(&env.global, &None, &env.user),
    }
}

pub fn load_config<'a>(build_dir: &'_ Option<String>) -> Result<Config<'a>, Error> {
    let file = find_env_file()?;
    let env = Environment::load(&file)?;

    let mut heuristics = HashMap::<&str, HeuristicFn>::new();
    heuristics.insert(SSH_PUB, find_ssh_keys);
    heuristics.insert(SSH_PRIV, find_ssh_keys);

    let mut environment_variables = HashMap::new();
    environment_variables.insert(LOG_DIR, vec!["FFX_LOG_DIR", "HOME", "HOMEPATH"]);
    environment_variables.insert(LOG_ENABLED, vec!["FFX_LOG_ENABLED"]);

    Config::new(&env, build_dir, environment_variables, heuristics, argh::from_env())
}

fn find_ssh_keys(key: &str) -> Option<Value> {
    let k = if key == SSH_PUB { "authorized_keys" } else { "pkey" };
    match std::env::var("FUCHSIA_DIR") {
        Ok(r) => {
            if Path::new(&r).exists() {
                return Some(Value::String(String::from(format!("{}/.ssh/{}", r, k))));
            }
        }
        Err(_) => {
            if key != SSH_PUB {
                return None;
            }
        }
    }
    match std::env::var("HOME") {
        Ok(r) => {
            if Path::new(&r).exists() {
                Some(Value::String(String::from(format!("{}/.ssh/id_rsa.pub", r))))
            } else {
                None
            }
        }
        Err(_) => None,
    }
}
