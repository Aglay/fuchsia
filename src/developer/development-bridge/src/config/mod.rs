// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::config::api::ConfigLevel,
    crate::config::api::{PersistentConfig, ReadConfig, WriteConfig},
    crate::config::cache::load_config,
    crate::config::config::Config,
    crate::config::environment::Environment,
    crate::constants::ENV_FILE,
    anyhow::{anyhow, Error},
    serde_json::Value,
    std::{env, fs::File, io::Write, path::PathBuf},
};

pub mod args;
pub mod command;

mod api;
mod cache;
mod config;
mod env_var_config;
mod environment;
mod file_backed_config;
mod heuristic_config;
mod heuristic_fns;
mod persistent_config;
mod priority_config;
mod runtime_config;

#[cfg(target_os = "linux")]
mod linux;

#[cfg(not(target_os = "linux"))]
mod not_linux;

pub async fn get_config(name: &str) -> Result<Option<Value>, Error> {
    get_config_with_build_dir(name, &None).await
}

pub async fn get_config_with_build_dir(
    name: &str,
    build_dir: &Option<String>,
) -> Result<Option<Value>, Error> {
    let config = load_config(build_dir).await?;
    let read_guard = config.read().await;
    Ok((*read_guard).get(name))
}

pub async fn get_config_str(name: &str, default: &str) -> String {
    get_config(name)
        .await
        .unwrap_or(Some(Value::String(default.to_string())))
        .map_or(default.to_string(), |v| v.as_str().unwrap_or(default).to_string())
}

pub async fn get_config_bool(name: &str, default: bool) -> bool {
    get_config(name).await.unwrap_or(Some(Value::Bool(default))).map_or(default, |v| {
        v.as_bool().unwrap_or(match v {
            Value::String(s) => s.parse().unwrap_or(default),
            _ => default,
        })
    })
}

// TODO: remove dead code allowance when used (if ever)
#[allow(dead_code)]
pub async fn set_config(level: ConfigLevel, name: &str, value: Value) -> Result<(), Error> {
    set_config_with_build_dir(level, name, value, None).await
}

pub async fn set_config_with_build_dir(
    level: ConfigLevel,
    name: &str,
    value: Value,
    build_dir: Option<String>,
) -> Result<(), Error> {
    let config = load_config(&build_dir).await?;
    let mut write_guard = config.write().await;
    (*write_guard).set(&level, &name, value)?;
    save_config(&mut *write_guard, build_dir)
}

// TODO: remove dead code allowance when used (if ever)
#[allow(dead_code)]
pub async fn remove_config(level: ConfigLevel, name: &str) -> Result<(), Error> {
    remove_config_with_build_dir(level, name, None).await
}

pub async fn remove_config_with_build_dir(
    level: ConfigLevel,
    name: &str,
    build_dir: Option<String>,
) -> Result<(), Error> {
    let config = load_config(&build_dir).await?;
    let mut write_guard = config.write().await;
    (*write_guard).remove(&level, &name)?;
    save_config(&mut *write_guard, build_dir)
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
