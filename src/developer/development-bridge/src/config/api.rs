// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {anyhow::Error, serde_json::Value};

#[derive(Debug, PartialEq, Copy, Clone)]
pub enum ConfigLevel {
    Defaults,
    Build,
    Global,
    User,
}

pub trait ReadConfig {
    fn get(&self, key: &str) -> Option<Value>;
}

pub trait WriteConfig {
    fn set(&mut self, level: &ConfigLevel, key: &str, value: Value) -> Result<(), Error>;
    fn remove(&mut self, level: &ConfigLevel, key: &str) -> Result<(), Error>;
}

pub trait PersistentConfig {
    fn save(
        &self,
        global: &Option<String>,
        build: &Option<&String>,
        user: &Option<String>,
    ) -> Result<(), Error>;
}
