// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::inspect::visit_system_objects,
    anyhow::{format_err, Error},
    std::{
        fs,
        path::{Path, PathBuf},
    },
};

type ComponentsResult = Result<Vec<V1Component>, Error>;
type RealmsResult = Result<Vec<V1Realm>, Error>;
type DirEntryResult = Result<Vec<fs::DirEntry>, Error>;

static SPACER: &str = "  ";
static UNKNOWN: &str = "UNKNOWN";

/// Used as a helper function to traverse <realm id>/r/, <realm id>/c/,
/// <component instance id>/c/, following through into their id subdirectories.
fn find_id_directories(dir: &Path) -> DirEntryResult {
    let entries = fs::read_dir(dir)?;
    let mut vec = vec![];
    for entry in entries {
        let entry = entry?;
        let path = entry.path();
        let id = {
            let name = path.file_name().ok_or_else(|| format_err!("no filename"))?;
            name.to_string_lossy()
        };

        // check for numeric directory name.
        if id.chars().all(char::is_numeric) {
            vec.push(entry)
        }
    }
    match !vec.is_empty() {
        true => Ok(vec),
        false => Err(format_err!("Directory not found")),
    }
}

fn visit_child_realms(realm_path: &Path) -> RealmsResult {
    let child_realms_path = realm_path.join("r");
    let entries = fs::read_dir(child_realms_path)?;
    let mut child_realms: Vec<V1Realm> = Vec::new();

    // visit all entries within <realm id>/r/
    for entry in entries {
        let entry = entry?;
        // visit <realm id>/r/<child realm name>/<child realm id>/
        let child_realm_id_dir_entries = find_id_directories(&entry.path())?;
        for child_realm_id_dir_entry in child_realm_id_dir_entries {
            let path = child_realm_id_dir_entry.path();
            child_realms.push(V1Realm::create(&path)?);
        }
    }
    Ok(child_realms)
}

/// Traverses a directory of named components, and recurses into each component directory.
/// Each component visited is added to the |child_components| vector.
fn visit_child_components(parent_path: &Path) -> ComponentsResult {
    let child_components_path = parent_path.join("c");
    if !child_components_path.is_dir() {
        return Ok(vec![]);
    }

    let mut child_components: Vec<V1Component> = Vec::new();
    let entries = fs::read_dir(&child_components_path)?;
    for entry in entries {
        let entry = entry?;
        // Visits */c/<component name>/<component instance id>.
        let component_instance_id_dir_entries = find_id_directories(&entry.path())?;
        for component_instance_id_dir_entry in component_instance_id_dir_entries {
            let path = component_instance_id_dir_entry.path();
            child_components.push(V1Component::create(path)?);
        }
    }
    Ok(child_components)
}

pub struct V1Realm {
    job_id: u32,
    name: String,
    child_realms: Vec<V1Realm>,
    child_components: Vec<V1Component>,
}

impl V1Realm {
    pub fn create(realm_path: impl AsRef<Path>) -> Result<V1Realm, Error> {
        let job_id = fs::read_to_string(&realm_path.as_ref().join("job-id"))?;
        let name = fs::read_to_string(&realm_path.as_ref().join("name"))?;
        Ok(V1Realm {
            job_id: job_id.parse::<u32>()?,
            name,
            child_realms: visit_child_realms(&realm_path.as_ref())?,
            child_components: visit_child_components(&realm_path.as_ref())?,
        })
    }

    pub fn generate_tree_recursive(&self, level: usize, lines: &mut Vec<String>) {
        let space = SPACER.repeat(level - 1);
        let line = format!("{}{} (realm)", space, self.name);
        lines.push(line);
        for child in &self.child_components {
            child.generate_tree_recursive(level + 1, lines);
        }
        for child in &self.child_realms {
            child.generate_tree_recursive(level + 1, lines);
        }
    }

    pub fn generate_details_recursive(&self, prefix: &str, lines: &mut Vec<String>) {
        let moniker = format!("{}{}", prefix, self.name);

        lines.push(moniker.clone());
        lines.push(format!("- Job ID: {}", self.job_id));
        lines.push(format!("- Type: v1 realm"));

        // Recurse on children
        let prefix = format!("{}/", moniker);
        for child in &self.child_components {
            lines.push("".to_string());
            child.generate_details_recursive(&prefix, lines);
        }

        for child in &self.child_realms {
            lines.push("".to_string());
            child.generate_details_recursive(&prefix, lines);
        }
    }

    pub fn inspect(&self, job_id: u32, exclude_objects: &Vec<String>) {
        for component in &self.child_components {
            component.inspect(job_id, exclude_objects);
        }
        for child_realm in &self.child_realms {
            child_realm.inspect(job_id, exclude_objects);
        }
    }
}

pub struct V1Component {
    job_id: u32,
    name: String,
    path: PathBuf,
    url: String,
    merkleroot: Option<String>,
    child_components: Vec<V1Component>,
}

impl V1Component {
    fn create(path: PathBuf) -> Result<V1Component, Error> {
        let job_id = fs::read_to_string(&path.join("job-id"))?;
        let url = fs::read_to_string(&path.join("url"))?;
        let name = fs::read_to_string(&path.join("name"))?;
        let merkleroot = fs::read_to_string(&path.join("in/pkg/meta")).ok();
        let child_components = visit_child_components(&path)?;
        Ok(V1Component {
            job_id: job_id.parse::<u32>()?,
            name,
            path,
            url,
            merkleroot,
            child_components,
        })
    }

    fn generate_tree_recursive(&self, level: usize, lines: &mut Vec<String>) {
        let space = SPACER.repeat(level - 1);
        let line = format!("{}{}", space, self.name);
        lines.push(line);
        for child in &self.child_components {
            child.generate_tree_recursive(level + 1, lines);
        }
    }

    fn generate_details_recursive(&self, prefix: &str, lines: &mut Vec<String>) {
        let moniker = format!("{}{}", prefix, self.name);
        let unknown_merkle = UNKNOWN.to_string();
        let merkle = self.merkleroot.as_ref().unwrap_or(&unknown_merkle);

        lines.push(moniker.clone());
        lines.push(format!("- URL: {}", self.url));
        lines.push(format!("- Job ID: {}", self.job_id));
        lines.push(format!("- Merkle Root: {}", merkle));
        lines.push(format!("- Type: v1 component"));

        // Recurse on children
        let prefix = format!("{}/", moniker);
        for child in &self.child_components {
            lines.push("".to_string());
            child.generate_details_recursive(&prefix, lines);
        }
    }

    fn inspect(&self, job_id: u32, exclude_objects: &Vec<String>) {
        if self.job_id == job_id {
            println!("{}[{}]: {}", self.name, self.job_id, self.url,);
            visit_system_objects(&self.path, exclude_objects)
                .expect("Failed to visit system objects");
        }
        for component in &self.child_components {
            component.inspect(job_id, exclude_objects);
        }
    }
}
