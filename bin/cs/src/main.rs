// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{err_msg, Error};
use inspect::{generate_inspect_object_tree, InspectObject, InspectValue};
use std::{
    fmt, fs,
    path::{Path, PathBuf},
};
use structopt::StructOpt;

mod inspect;

type ComponentsResult = Result<Vec<Component>, Error>;
type RealmsResult = Result<Vec<Realm>, Error>;
type TraversalResult = Result<(), Error>;
type DirEntryResult = Result<fs::DirEntry, Error>;

struct Realm {
    job_id: u32,
    name: String,
    child_realms: Vec<Realm>,
    child_components: Vec<Component>,
}

impl Realm {
    fn create(realm_path: impl AsRef<Path>) -> Result<Realm, Error> {
        let job_id = fs::read_to_string(&realm_path.as_ref().join("job-id"))?;
        let name = fs::read_to_string(&realm_path.as_ref().join("name"))?;
        let realm = Realm {
            job_id: job_id.parse::<u32>()?,
            name: name,
            child_realms: visit_child_realms(&realm_path.as_ref())?,
            child_components: visit_child_components(&realm_path.as_ref())?,
        };
        Ok(realm)
    }

    fn write_indented(&self, f: &mut fmt::Formatter, indent: usize) -> fmt::Result {
        writeln!(
            f,
            "{}Realm[{}]: {}",
            " ".repeat(indent),
            self.job_id,
            self.name,
        )?;

        for comp in &self.child_components {
            comp.write_indented(f, indent + 2)?;
        }

        for realm in &self.child_realms {
            realm.write_indented(f, indent + 2)?;
        }

        Ok(())
    }
}

impl fmt::Display for Realm {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.write_indented(f, 0)
    }
}

struct Component {
    job_id: u32,
    name: String,
    path: PathBuf,
    url: String,
    child_components: Vec<Component>,
}

impl Component {
    fn create(component_path: &Path) -> Result<Component, Error> {
        let job_id = fs::read_to_string(&component_path.join("job-id"))?;
        let url = fs::read_to_string(&component_path.join("url"))?;
        let name = fs::read_to_string(&component_path.join("name"))?;
        let component = Component {
            job_id: job_id.parse::<u32>()?,
            name: name,
            path: component_path.to_path_buf(),
            url: url,
            child_components: visit_child_components(&component_path)?,
        };
        Ok(component)
    }

    fn write_indented(&self, f: &mut fmt::Formatter, indent: usize) -> fmt::Result {
        writeln!(
            f,
            "{}{}[{}]: {}",
            " ".repeat(indent),
            self.name,
            self.job_id,
            self.url,
        )?;

        for child in &self.child_components {
            child.write_indented(f, indent + 2)?;
        }

        Ok(())
    }
}

impl fmt::Display for Component {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.write_indented(f, 0)
    }
}

fn visit_child_realms(realm_path: &Path) -> RealmsResult {
    let child_realms_path = realm_path.join("r");
    let entries = fs::read_dir(child_realms_path)?;
    let mut child_realms: Vec<Realm> = Vec::new();

    // visit all entries within <realm id>/r/
    for entry in entries {
        let entry = entry?;
        // visit <realm id>/r/<child realm name>/<child realm id>/
        let child_realm_id_dir_entry = find_id_directory(&entry.path())?;
        let path = child_realm_id_dir_entry.path();
        child_realms.push(Realm::create(&path)?);
    }
    Ok(child_realms)
}

/// Used as a helper function to traverse <realm id>/r/, <realm id>/c/,
/// <component instance id>/c/, following through into their id subdirectories.
fn find_id_directory(dir: &Path) -> DirEntryResult {
    let entries = fs::read_dir(dir)?;
    for entry in entries {
        let entry = entry?;
        let path = entry.path();
        let id = {
            let name = path.file_name().ok_or_else(|| err_msg("no filename"))?;
            name.to_string_lossy()
        };

        // Find the first numeric directory name and return its directory entry.
        if id.chars().all(char::is_numeric) {
            return Ok(entry);
        }
    }
    return Err(err_msg("Directory not found"));
}

fn visit_threads(component_path: &Path, exclude_properties: &Vec<String>) -> TraversalResult {
    let channel_path = component_path.join("system_objects/threads/.channel");
    let inspect_object = generate_inspect_object_tree(&channel_path, &exclude_properties)?;
    visit_inspect_object(0, &inspect_object);
    Ok(())
}

fn visit_inspect_object(depth: usize, inspect_object: &InspectObject) {
    let indent = " ".repeat(depth);
    println!("{}{}", indent, inspect_object.name);
    for property in &inspect_object.properties {
        println!(
            "{} {}:{}",
            indent,
            property.key,
            match &property.value {
                InspectValue::Text(s) => s.clone(),
                InspectValue::Binary => String::from("<binary>"),
            },
        );
    }
    for child in &inspect_object.child_inspect_objects {
        visit_inspect_object(depth + 1, child);
    }
}

/// Traverses a directory of named components, and recurses into each component directory.
/// Each component visited is added to the |child_components| vector.
fn visit_child_components(parent_path: &Path) -> ComponentsResult {
    let child_components_path = parent_path.join("c");
    if !child_components_path.is_dir() {
        return Ok(vec![]);
    }

    let mut child_components: Vec<Component> = Vec::new();
    let entries = fs::read_dir(&child_components_path)?;
    for entry in entries {
        let entry = entry?;
        // Visits */c/<component name>/<component instance id>.
        let component_instance_id_dir_entry = find_id_directory(&entry.path())?;
        let path = component_instance_id_dir_entry.path();
        child_components.push(Component::create(&path)?);
    }
    Ok(child_components)
}

fn inspect_realm(job_id: u32, exclude_properties: &Vec<String>, realm: &Realm) -> TraversalResult {
    for component in &realm.child_components {
        inspect_component(job_id, exclude_properties, component)?;
    }
    for child_realm in &realm.child_realms {
        inspect_realm(job_id, exclude_properties, child_realm)?;
    }
    Ok(())
}

fn inspect_component(
    job_id: u32, exclude_properties: &Vec<String>, component: &Component,
) -> TraversalResult {
    if component.job_id == job_id {
        visit_threads(&component.path, exclude_properties)?;
    }
    for component in &component.child_components {
        inspect_component(job_id, exclude_properties, component)?;
    }
    Ok(())
}

#[derive(StructOpt, Debug)]
#[structopt(
    name = "Component Statistics (cs) Reporting Tool",
    about = "Displays information about components on the system."
)]
struct Opt {
    /// Recursively output the Inspect trees of the components in the provided
    /// job Id.
    #[structopt(short = "i", long = "inspect")]
    job_id: Option<u32>,

    // Properties to exclude from display when presenting Inspect trees.
    #[structopt(
        short = "e",
        long = "exclude-properties",
        raw(use_delimiter = "true"),
        default_value = "stack,stacks"
    )]
    exclude_properties: Vec<String>,
}

fn main() -> TraversalResult {
    // Visit the directory /hub and recursively traverse it, outputting
    // information about the component hierarchy.
    // See https://fuchsia.googlesource.com/docs/+/master/the-book/hub.md for
    // more information on the Hub directory structure.
    let opt = Opt::from_args();
    let root_realm = Realm::create("/hub")?;
    match opt.job_id {
        Some(job_id) => inspect_realm(job_id, &opt.exclude_properties, &root_realm)?,
        _ => println!("{}", root_realm),
    };
    Ok(())
}
