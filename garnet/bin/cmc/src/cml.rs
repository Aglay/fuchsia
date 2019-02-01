// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use lazy_static::lazy_static;
use regex::Regex;
use serde_derive::Deserialize;
use serde_json::{Map, Value};

pub const DIRECTORY: &str = "directory";
pub const SERVICE: &str = "service";
lazy_static! {
    pub static ref CHILD_RE: Regex = Regex::new(r"^#([A-Za-z0-9\-_]+)$").unwrap();
    pub static ref FROM_RE: Regex = Regex::new(r"^(realm|self|#[A-Za-z0-9\-_]+)$").unwrap();
}

#[derive(Deserialize, Debug)]
pub struct Document {
    pub program: Option<Map<String, Value>>,
    pub r#use: Option<Vec<Use>>,
    pub expose: Option<Vec<Expose>>,
    pub offer: Option<Vec<Offer>>,
    pub children: Option<Vec<Child>>,
    pub facets: Option<Map<String, Value>>,
}

#[derive(Deserialize, Debug)]
pub struct Use {
    pub service: Option<String>,
    pub directory: Option<String>,
    pub r#as: Option<String>,
}

#[derive(Deserialize, Debug)]
pub struct Expose {
    pub service: Option<String>,
    pub directory: Option<String>,
    pub from: String,
    pub r#as: Option<String>,
}

#[derive(Deserialize, Debug)]
pub struct Offer {
    pub service: Option<String>,
    pub directory: Option<String>,
    pub from: String,
    pub targets: Vec<Target>,
}

#[derive(Deserialize, Debug)]
pub struct Target {
    pub to: String,
    pub r#as: Option<String>,
}

#[derive(Deserialize, Debug)]
pub struct Child {
    pub name: String,
    pub uri: String,
}

pub trait FromClause {
    fn from(&self) -> &str;
}

pub trait CapabilityClause {
    fn service(&self) -> &Option<String>;
    fn directory(&self) -> &Option<String>;
}

pub trait AsClause {
    fn r#as(&self) -> &Option<String>;
}
pub trait ToClause {
    fn to(&self) -> Option<&str>;
}

impl CapabilityClause for Use {
    fn service(&self) -> &Option<String> {
        &self.service
    }
    fn directory(&self) -> &Option<String> {
        &self.directory
    }
}

impl AsClause for Use {
    fn r#as(&self) -> &Option<String> {
        &self.r#as
    }
}

impl FromClause for Expose {
    fn from(&self) -> &str {
        &self.from
    }
}

impl CapabilityClause for Expose {
    fn service(&self) -> &Option<String> {
        &self.service
    }
    fn directory(&self) -> &Option<String> {
        &self.directory
    }
}

impl AsClause for Expose {
    fn r#as(&self) -> &Option<String> {
        &self.r#as
    }
}

impl ToClause for Expose {
    fn to(&self) -> Option<&str> {
        None
    }
}

impl FromClause for Offer {
    fn from(&self) -> &str {
        &self.from
    }
}

impl CapabilityClause for Offer {
    fn service(&self) -> &Option<String> {
        &self.service
    }
    fn directory(&self) -> &Option<String> {
        &self.directory
    }
}

impl AsClause for Target {
    fn r#as(&self) -> &Option<String> {
        &self.r#as
    }
}

impl ToClause for Target {
    fn to(&self) -> Option<&str> {
        Some(&self.to)
    }
}
