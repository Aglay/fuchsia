// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    scrutiny::prelude::*,
    scrutiny_utils::zbi::ZbiSection,
    serde::{Deserialize, Serialize},
    std::collections::HashMap,
    uuid::Uuid,
};

/// Defines a component. Each component has a unique id which is used to link
/// it in the Route table. Each component also has a url and a version. This
/// structure is intended to be lightweight and general purpose if you need to
/// append additional information about a component make another table and
/// index it on the `component.id`.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Component {
    pub id: i32,
    pub url: String,
    pub version: i32,
    pub inferred: bool,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Components {
    pub entries: Vec<Component>,
}

impl DataCollection for Components {
    fn uuid() -> Uuid {
        Uuid::parse_str("559f0e26-5ff2-45ce-a5e8-ce0281da8681").unwrap()
    }
}

/// Defines a fuchsia package. Each package has a unique url. This provides an
/// expanded meta/contents so you can see all of the files defined in this
/// package.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Package {
    pub url: String,
    pub merkle: String,
    pub contents: HashMap<String, String>,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Packages {
    pub entries: Vec<Package>,
}

impl DataCollection for Packages {
    fn uuid() -> Uuid {
        Uuid::parse_str("80d8b6ab-6ba5-45bc-9461-ba9cc9e0c55b").unwrap()
    }
}

/// A component instance is a specific instantiation of a component. These
/// may run in a particular realm with certain restrictions.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct ComponentInstance {
    pub id: i32,
    pub moniker: String,
    pub component_id: i32,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct ComponentInstances {
    pub entries: Vec<ComponentInstance>,
}

impl DataCollection for ComponentInstances {
    fn uuid() -> Uuid {
        Uuid::parse_str("d621f0a5-79e2-432d-8954-f5c9923c0544").unwrap()
    }
}

/// Defines the manifest data in terms of the component framework version it
/// represents.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub enum ManifestData {
    Version1(String),
    Version2(String),
}
/// Defines a component manifest. The `component_id` maps 1:1 to
/// `component.id` indexes. This is stored in a different table as most queries
/// don't need the raw manifest.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Manifest {
    pub component_id: i32,
    pub manifest: ManifestData,
    pub uses: Vec<String>,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Manifests {
    pub entries: Vec<Manifest>,
}

impl DataCollection for Manifests {
    fn uuid() -> Uuid {
        Uuid::parse_str("324da08b-5ab8-43f1-8ff1-4687f32c7712").unwrap()
    }
}

// TODO(benwright) - Add support for "first class" capabilities such as runners,
// resolvers and events.
/// Defines a link between two components. The `src_id` is the `component_instance.id`
/// of the component giving a service or directory to the `dst_id`. The
/// `protocol_id` refers to the Protocol with this link.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Route {
    pub id: i32,
    pub src_id: i32,
    pub dst_id: i32,
    pub service_name: String,
    pub protocol_id: i32,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Routes {
    pub entries: Vec<Route>,
}

impl DataCollection for Routes {
    fn uuid() -> Uuid {
        Uuid::parse_str("6def84c2-afea-458d-bd36-7dc550e84e90").unwrap()
    }
}

/// Defines either a FIDL or Directory protocol with some interface name such
/// as fuchshia.foo.Bar and an optional path such as "/dev".
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Protocol {
    pub id: i32,
    pub interface: String,
    pub path: String,
}

#[derive(Default, Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Protocols {
    pub entries: Vec<Protocol>,
}

impl DataCollection for Protocols {
    fn uuid() -> Uuid {
        Uuid::parse_str("8a14a6ce-3357-43d7-b4fb-7e005062dfda").unwrap()
    }
}

/// Defines all of the parsed information in the ZBI.
#[derive(Deserialize, Serialize, Debug, Clone, PartialEq, Eq)]
pub struct Zbi {
    // Raw section data for each zbi section. This section isn't serialized to
    // disk because it occupies a large amount of space.
    #[serde(skip)]
    pub sections: Vec<ZbiSection>,
    // File names to data contained in bootfs.
    // TODO(benwright) - Work out how to optimize this for speed.
    #[serde(skip)]
    pub bootfs: HashMap<String, Vec<u8>>,
    pub cmdline: String,
}

impl DataCollection for Zbi {
    fn uuid() -> Uuid {
        Uuid::parse_str("df9ec25f-63b7-4d88-8e79-5ff9deb0afa8").unwrap()
    }
}
