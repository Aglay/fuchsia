// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{
        metrics::Metrics,
        validate::{self, Number, ROOT_ID},
        DiffType,
    },
    anyhow::{bail, format_err, Error},
    difference,
    fuchsia_inspect::{self, format::block::ArrayFormat},
    fuchsia_inspect_node_hierarchy::LinkNodeDisposition,
    num_derive::{FromPrimitive, ToPrimitive},
    std::{
        self,
        collections::{HashMap, HashSet},
        convert::TryInto,
    },
};

mod scanner;
pub use scanner::Scanner;
mod fetch;
pub use fetch::LazyNode;

#[cfg(test)]
use num_traits::ToPrimitive;

const ROOT_NAME: &str = "root";

/// A local store of Inspect-like data which can be built by Action or filled
/// from a VMO.
///
/// For now, Data assumes it will not be given two sibling-nodes or
/// properties with the same name, and does not truncate any data or names.
#[derive(Debug)]
pub struct Data {
    nodes: HashMap<u32, Node>,
    properties: HashMap<u32, Property>,
    tombstone_nodes: HashSet<u32>,
    tombstone_properties: HashSet<u32>,
}

// Data is the only public struct in this file. The internal data structures are
// a bit complicated...
//
// Node, Property, and Payload are the "clean data" holders - they can be created
// either by reading from a VMO, or by applying Actions (the same actions that
// are sent to the puppets and act on their VMOs).
//
// The Actions specify arbitrary u32 keys to refer to nodes and properties to
// create, modify, and delete. It's an error to misuse a key (e.g. double-create
// a key or delete a missing key).
//
// In both Data-from-Actions and Data-from-VMO, the "root" node is virtual; nodes
// and properties with a "parent" ID of 0 are directly under the "root" of the tree.
// A placeholder Node is placed at index 0 upon creation to hold the real Nodes and
// properties added during scanning VMO or processing Actions.

#[derive(Debug)]
pub struct Node {
    name: String,
    parent: u32,
    children: HashSet<u32>,
    properties: HashSet<u32>,
}

#[derive(Debug)]
pub struct Property {
    name: String,
    id: u32,
    parent: u32,
    payload: Payload,
}

#[derive(Debug)]
enum Payload {
    String(String),
    Bytes(Vec<u8>),
    Int(i64),
    Uint(u64),
    Double(f64),
    Bool(bool),
    IntArray(Vec<i64>, ArrayFormat),
    UintArray(Vec<u64>, ArrayFormat),
    DoubleArray(Vec<f64>, ArrayFormat),
    Link { disposition: LinkNodeDisposition, parsed_data: Data },
}

impl Property {
    fn to_string(&self, prefix: &str) -> String {
        match &self.payload {
            Payload::Link { disposition, parsed_data } => match disposition {
                LinkNodeDisposition::Child => {
                    format!("{} {}: {}", prefix, self.name, parsed_data.to_string_internal(true))
                }
                LinkNodeDisposition::Inline => format!("{}", parsed_data.to_string_internal(true)),
            },
            _ => format!("{} {}: {:?}", prefix, self.name, &self.payload),
        }
    }
}

impl Node {
    /// If `hide_root` is true and the node is the root,
    /// then the name and and prefix of the generated string is omitted.
    /// This is used for lazy nodes wherein we don't what to show the label "root" for lazy nodes.
    fn to_string(&self, prefix: &str, tree: &Data, hide_root: bool) -> String {
        let sub_prefix = format!("{}> ", prefix);
        let mut nodes = vec![];
        for node_id in self.children.iter() {
            nodes.push(
                tree.nodes
                    .get(node_id)
                    .map_or("Missing child".into(), |n| n.to_string(&sub_prefix, tree, hide_root)),
            );
        }
        let mut properties = vec![];
        for property_id in self.properties.iter() {
            properties.push(
                tree.properties
                    .get(property_id)
                    .map_or("Missing property".into(), |p| p.to_string(&sub_prefix)),
            );
        }
        nodes.sort_unstable();
        properties.sort_unstable();
        if self.name == ROOT_NAME && hide_root {
            format!("{}\n{}\n", properties.join("\n"), nodes.join("\n"))
        } else {
            format!(
                "{} {} ->\n{}\n{}\n",
                prefix,
                self.name,
                properties.join("\n"),
                nodes.join("\n")
            )
        }
    }
}

struct Op {
    int: fn(i64, i64) -> i64,
    uint: fn(u64, u64) -> u64,
    double: fn(f64, f64) -> f64,
    name: &'static str,
}

const ADD: Op = Op { int: |a, b| a + b, uint: |a, b| a + b, double: |a, b| a + b, name: "add" };
const SUBTRACT: Op =
    Op { int: |a, b| a - b, uint: |a, b| a - b, double: |a, b| a - b, name: "subtract" };
const SET: Op = Op { int: |_a, b| b, uint: |_a, b| b, double: |_a, b| b, name: "set" };

macro_rules! insert_linear_fn {
    ($name:ident, $type:ident) => {
        fn $name(numbers: &mut Vec<$type>, value: $type, count: u64) -> Result<(), Error> {
            let buckets: $type = (numbers.len() as i32 - 4).try_into().unwrap();
            let floor = numbers[0];
            let step_size = numbers[1];
            let index: usize = if value < floor {
                2
            } else if value >= floor + buckets * step_size {
                numbers.len() - 1
            } else {
                (((value - floor) / step_size) as $type + 3 as $type) as i32 as usize
            };
            numbers[index] += count as $type;
            Ok(())
        }
    };
}

insert_linear_fn! {insert_linear_i, i64}
insert_linear_fn! {insert_linear_u, u64}
insert_linear_fn! {insert_linear_d, f64}

// DO NOT USE this algorithm in non-test libraries!
// It's good to implement the test with a different algorithm than the code being tested.
// But this is a BAD algorithm in real life.
// 1) Too many casts - extreme values may not be handled correctly.
// 2) Floating point math is imprecise; int/uint values over 2^56 or so won't be
//     calculated correctly because they can't be expressed precisely, and the log2/log2
//     division may come down on the wrong side of the bucket boundary. That's why there's
//     a fudge factor added to int results - but that's only correct up to a million or so.
macro_rules! insert_exponential_fn {
    ($name:ident, $type:ident, $fudge_factor:expr) => {
        fn $name(numbers: &mut Vec<$type>, value: $type, count: u64) -> Result<(), Error> {
            let buckets = numbers.len() - 5;
            let floor = numbers[0];
            let initial_step = numbers[1];
            let step_multiplier = numbers[2];
            let index = if value < floor {
                3
            } else if value < floor + initial_step {
                4
            } else if value
                >= floor + initial_step * (step_multiplier as f64).powi(buckets as i32 - 1) as $type
            {
                numbers.len() - 1
            } else {
                ((((value as f64 - floor as f64) / initial_step as f64) as f64).log2()
                    / (step_multiplier as f64 + $fudge_factor).log2())
                .trunc() as usize
                    + 5
            };
            numbers[index] += count as $type;
            Ok(())
        }
    };
}

insert_exponential_fn! {insert_exponential_i, i64, 0.0000000000000000000001}
insert_exponential_fn! {insert_exponential_u, u64, 0.0000000000000000000001}
insert_exponential_fn! {insert_exponential_d, f64, 0.0}

impl Data {
    // ***** Here are the functions to apply Actions to a Data.

    /// Applies the given action to this in-memory state.
    pub fn apply(&mut self, action: &validate::Action) -> Result<(), Error> {
        match action {
            validate::Action::CreateNode(validate::CreateNode { parent, id, name }) => {
                self.create_node(*parent, *id, name)
            }
            validate::Action::DeleteNode(validate::DeleteNode { id }) => self.delete_node(*id),
            validate::Action::CreateNumericProperty(validate::CreateNumericProperty {
                parent,
                id,
                name,
                value,
            }) => self.add_property(
                *parent,
                *id,
                name,
                match value {
                    validate::Number::IntT(value) => Payload::Int(*value),
                    validate::Number::UintT(value) => Payload::Uint(*value),
                    validate::Number::DoubleT(value) => Payload::Double(*value),
                    unknown => return Err(format_err!("Unknown number type {:?}", unknown)),
                },
            ),
            validate::Action::CreateBytesProperty(validate::CreateBytesProperty {
                parent,
                id,
                name,
                value,
            }) => self.add_property(*parent, *id, name, Payload::Bytes(value.clone())),
            validate::Action::CreateStringProperty(validate::CreateStringProperty {
                parent,
                id,
                name,
                value,
            }) => self.add_property(*parent, *id, name, Payload::String(value.to_string())),
            validate::Action::CreateBoolProperty(validate::CreateBoolProperty {
                parent,
                id,
                name,
                value,
            }) => self.add_property(*parent, *id, name, Payload::Bool(*value)),
            validate::Action::DeleteProperty(validate::DeleteProperty { id }) => {
                self.delete_property(*id)
            }
            validate::Action::SetNumber(validate::SetNumber { id, value }) => {
                self.modify_number(*id, value, SET)
            }
            validate::Action::AddNumber(validate::AddNumber { id, value }) => {
                self.modify_number(*id, value, ADD)
            }
            validate::Action::SubtractNumber(validate::SubtractNumber { id, value }) => {
                self.modify_number(*id, value, SUBTRACT)
            }
            validate::Action::SetBytes(validate::SetBytes { id, value }) => {
                self.set_bytes(*id, value)
            }
            validate::Action::SetString(validate::SetString { id, value }) => {
                self.set_string(*id, value)
            }
            validate::Action::SetBool(validate::SetBool { id, value }) => {
                self.set_bool(*id, *value)
            }
            validate::Action::CreateArrayProperty(validate::CreateArrayProperty {
                parent,
                id,
                name,
                slots,
                number_type,
            }) => self.add_property(
                *parent,
                *id,
                name,
                match number_type {
                    validate::NumberType::Int => {
                        Payload::IntArray(vec![0; *slots as usize], ArrayFormat::Default)
                    }
                    validate::NumberType::Uint => {
                        Payload::UintArray(vec![0; *slots as usize], ArrayFormat::Default)
                    }
                    validate::NumberType::Double => {
                        Payload::DoubleArray(vec![0.0; *slots as usize], ArrayFormat::Default)
                    }
                },
            ),
            validate::Action::ArrayAdd(validate::ArrayAdd { id, index, value }) => {
                self.modify_array(*id, *index, value, ADD)
            }
            validate::Action::ArraySubtract(validate::ArraySubtract { id, index, value }) => {
                self.modify_array(*id, *index, value, SUBTRACT)
            }
            validate::Action::ArraySet(validate::ArraySet { id, index, value }) => {
                self.modify_array(*id, *index, value, SET)
            }
            validate::Action::CreateLinearHistogram(validate::CreateLinearHistogram {
                parent,
                id,
                name,
                floor,
                step_size,
                buckets,
            }) => self.add_property(
                *parent,
                *id,
                name,
                match (floor, step_size) {
                    (validate::Number::IntT(floor), validate::Number::IntT(step_size)) => {
                        let mut data = vec![0; *buckets as usize + 4];
                        data[0] = *floor;
                        data[1] = *step_size;
                        Payload::IntArray(data, ArrayFormat::LinearHistogram)
                    }
                    (validate::Number::UintT(floor), validate::Number::UintT(step_size)) => {
                        let mut data = vec![0; *buckets as usize + 4];
                        data[0] = *floor;
                        data[1] = *step_size;
                        Payload::UintArray(data, ArrayFormat::LinearHistogram)
                    }
                    (validate::Number::DoubleT(floor), validate::Number::DoubleT(step_size)) => {
                        let mut data = vec![0.0; *buckets as usize + 4];
                        data[0] = *floor;
                        data[1] = *step_size;
                        Payload::DoubleArray(data, ArrayFormat::LinearHistogram)
                    }
                    unexpected => {
                        return Err(format_err!(
                            "Bad types in CreateLinearHistogram: {:?}",
                            unexpected
                        ))
                    }
                },
            ),
            validate::Action::CreateExponentialHistogram(
                validate::CreateExponentialHistogram {
                    parent,
                    id,
                    name,
                    floor,
                    initial_step,
                    step_multiplier,
                    buckets,
                },
            ) => self.add_property(
                *parent,
                *id,
                name,
                match (floor, initial_step, step_multiplier) {
                    (
                        validate::Number::IntT(floor),
                        validate::Number::IntT(initial_step),
                        validate::Number::IntT(step_multiplier),
                    ) => {
                        let mut data = vec![0i64; *buckets as usize + 5];
                        data[0] = *floor;
                        data[1] = *initial_step;
                        data[2] = *step_multiplier;
                        Payload::IntArray(data, ArrayFormat::ExponentialHistogram)
                    }
                    (
                        validate::Number::UintT(floor),
                        validate::Number::UintT(initial_step),
                        validate::Number::UintT(step_multiplier),
                    ) => {
                        let mut data = vec![0u64; *buckets as usize + 5];
                        data[0] = *floor;
                        data[1] = *initial_step;
                        data[2] = *step_multiplier;
                        Payload::UintArray(data, ArrayFormat::ExponentialHistogram)
                    }
                    (
                        validate::Number::DoubleT(floor),
                        validate::Number::DoubleT(initial_step),
                        validate::Number::DoubleT(step_multiplier),
                    ) => {
                        let mut data = vec![0.0f64; *buckets as usize + 5];
                        data[0] = *floor;
                        data[1] = *initial_step;
                        data[2] = *step_multiplier;
                        Payload::DoubleArray(data, ArrayFormat::ExponentialHistogram)
                    }
                    unexpected => {
                        return Err(format_err!(
                            "Bad types in CreateExponentialHistogram: {:?}",
                            unexpected
                        ))
                    }
                },
            ),
            validate::Action::Insert(validate::Insert { id, value }) => {
                if let Some(mut property) = self.properties.get_mut(&id) {
                    match (&mut property, value) {
                        (
                            Property {
                                payload: Payload::IntArray(numbers, ArrayFormat::LinearHistogram),
                                ..
                            },
                            Number::IntT(value),
                        ) => insert_linear_i(numbers, *value, 1),
                        (
                            Property {
                                payload:
                                    Payload::IntArray(numbers, ArrayFormat::ExponentialHistogram),
                                ..
                            },
                            Number::IntT(value),
                        ) => insert_exponential_i(numbers, *value, 1),
                        (
                            Property {
                                payload: Payload::UintArray(numbers, ArrayFormat::LinearHistogram),
                                ..
                            },
                            Number::UintT(value),
                        ) => insert_linear_u(numbers, *value, 1),
                        (
                            Property {
                                payload:
                                    Payload::UintArray(numbers, ArrayFormat::ExponentialHistogram),
                                ..
                            },
                            Number::UintT(value),
                        ) => insert_exponential_u(numbers, *value, 1),
                        (
                            Property {
                                payload: Payload::DoubleArray(numbers, ArrayFormat::LinearHistogram),
                                ..
                            },
                            Number::DoubleT(value),
                        ) => insert_linear_d(numbers, *value, 1),
                        (
                            Property {
                                payload:
                                    Payload::DoubleArray(numbers, ArrayFormat::ExponentialHistogram),
                                ..
                            },
                            Number::DoubleT(value),
                        ) => insert_exponential_d(numbers, *value, 1),
                        unexpected => {
                            return Err(format_err!(
                                "Type mismatch {:?} trying to insert",
                                unexpected
                            ))
                        }
                    }
                } else {
                    return Err(format_err!(
                        "Tried to insert number on nonexistent property {}",
                        id
                    ));
                }
            }
            validate::Action::InsertMultiple(validate::InsertMultiple { id, value, count }) => {
                if let Some(mut property) = self.properties.get_mut(&id) {
                    match (&mut property, value) {
                        (
                            Property {
                                payload: Payload::IntArray(numbers, ArrayFormat::LinearHistogram),
                                ..
                            },
                            Number::IntT(value),
                        ) => insert_linear_i(numbers, *value, *count),
                        (
                            Property {
                                payload:
                                    Payload::IntArray(numbers, ArrayFormat::ExponentialHistogram),
                                ..
                            },
                            Number::IntT(value),
                        ) => insert_exponential_i(numbers, *value, *count),
                        (
                            Property {
                                payload: Payload::UintArray(numbers, ArrayFormat::LinearHistogram),
                                ..
                            },
                            Number::UintT(value),
                        ) => insert_linear_u(numbers, *value, *count),
                        (
                            Property {
                                payload:
                                    Payload::UintArray(numbers, ArrayFormat::ExponentialHistogram),
                                ..
                            },
                            Number::UintT(value),
                        ) => insert_exponential_u(numbers, *value, *count),
                        (
                            Property {
                                payload: Payload::DoubleArray(numbers, ArrayFormat::LinearHistogram),
                                ..
                            },
                            Number::DoubleT(value),
                        ) => insert_linear_d(numbers, *value, *count),
                        (
                            Property {
                                payload:
                                    Payload::DoubleArray(numbers, ArrayFormat::ExponentialHistogram),
                                ..
                            },
                            Number::DoubleT(value),
                        ) => insert_exponential_d(numbers, *value, *count),
                        unexpected => {
                            return Err(format_err!(
                                "Type mismatch {:?} trying to insert multiple",
                                unexpected
                            ))
                        }
                    }
                } else {
                    return Err(format_err!(
                        "Tried to insert_multiple number on nonexistent property {}",
                        id
                    ));
                }
            }
            _ => return Err(format_err!("Unknown action {:?}", action)),
        }
    }

    pub fn apply_lazy(&mut self, lazy_action: &validate::LazyAction) -> Result<(), Error> {
        match lazy_action {
            validate::LazyAction::CreateLazyNode(validate::CreateLazyNode {
                parent,
                id,
                name,
                disposition,
                actions,
            }) => self.add_lazy_node(*parent, *id, name, disposition, actions),
            validate::LazyAction::ModifyLazyNode(validate::ModifyLazyNode { id, actions }) => {
                self.modify_lazy_node(*id, actions)
            }
            validate::LazyAction::DeleteLazyNode(validate::DeleteLazyNode { id }) => {
                self.delete_property(*id)
            }
            _ => Err(format_err!("Unknown lazy action {:?}", lazy_action)),
        }
    }

    fn create_node(&mut self, parent: u32, id: u32, name: &str) -> Result<(), Error> {
        let node = Node {
            name: name.to_owned(),
            parent,
            children: HashSet::new(),
            properties: HashSet::new(),
        };
        if self.tombstone_nodes.contains(&id) {
            return Err(format_err!("Tried to create implicitly deleted node {}", id));
        }
        if let Some(_) = self.nodes.insert(id, node) {
            return Err(format_err!("Create called when node already existed at {}", id));
        }
        if let Some(parent_node) = self.nodes.get_mut(&parent) {
            parent_node.children.insert(id);
        } else {
            return Err(format_err!("Parent {} of created node {} doesn't exist", parent, id));
        }
        Ok(())
    }

    fn delete_node(&mut self, id: u32) -> Result<(), Error> {
        if id == 0 {
            return Err(format_err!("Do not try to delete node 0"));
        }
        if self.tombstone_nodes.remove(&id) {
            return Ok(());
        }
        if let Some(node) = self.nodes.remove(&id) {
            // Tombstone all descendents. An orphan descendent may reappear improperly if a new
            // node is created with a recycled ID.
            for child in node.children.clone().iter() {
                self.make_tombstone_node(*child)?;
            }
            for property in node.properties.clone().iter() {
                self.make_tombstone_property(*property)?;
            }
            if let Some(parent) = self.nodes.get_mut(&node.parent) {
                if !parent.children.remove(&id) {
                    // Some of these can only happen in case of internal logic errors.
                    // I can't think of a way to test them; I think the errors are
                    // actually impossible. Should I leave them untested? Remove them
                    // from the code? Add a special test_cfg make_illegal_node()
                    // function just to test them?
                    bail!(
                        "Internal error! Parent {} didn't know about this child {}",
                        node.parent,
                        id
                    );
                }
            }
        } else {
            return Err(format_err!("Delete of nonexistent node {}", id));
        }
        Ok(())
    }

    fn make_tombstone_node(&mut self, id: u32) -> Result<(), Error> {
        if id == 0 {
            return Err(format_err!("Internal error! Do not try to delete node 0."));
        }
        if let Some(node) = self.nodes.remove(&id) {
            for child in node.children.clone().iter() {
                self.make_tombstone_node(*child)?;
            }
            for property in node.properties.clone().iter() {
                self.make_tombstone_property(*property)?;
            }
        } else {
            return Err(format_err!("Internal error! Tried to tombstone nonexistent node {}", id));
        }
        self.tombstone_nodes.insert(id);
        Ok(())
    }

    fn make_tombstone_property(&mut self, id: u32) -> Result<(), Error> {
        if let None = self.properties.remove(&id) {
            return Err(format_err!(
                "Internal error! Tried to tombstone nonexistent property {}",
                id
            ));
        }
        self.tombstone_properties.insert(id);
        Ok(())
    }

    fn add_property(
        &mut self,
        parent: u32,
        id: u32,
        name: &str,
        payload: Payload,
    ) -> Result<(), Error> {
        if let Some(node) = self.nodes.get_mut(&parent) {
            node.properties.insert(id);
        } else {
            return Err(format_err!("Parent {} of property {} not found", parent, id));
        }
        if self.tombstone_properties.contains(&id) {
            return Err(format_err!("Tried to create implicitly deleted property {}", id));
        }
        let property = Property { parent, id, name: name.into(), payload };
        if let Some(_) = self.properties.insert(id, property) {
            return Err(format_err!("Property insert called on existing id {}", id));
        }
        Ok(())
    }

    fn delete_property(&mut self, id: u32) -> Result<(), Error> {
        if self.tombstone_properties.remove(&id) {
            return Ok(());
        }
        if let Some(property) = self.properties.remove(&id) {
            if let Some(node) = self.nodes.get_mut(&property.parent) {
                if !node.properties.remove(&id) {
                    bail!(
                        "Internal error! Property {}'s parent {} didn't have it as child",
                        id,
                        property.parent
                    );
                }
            } else {
                bail!(
                    "Internal error! Property {}'s parent {} doesn't exist on delete",
                    id,
                    property.parent
                );
            }
        } else {
            return Err(format_err!("Delete of nonexistent property {}", id));
        }
        Ok(())
    }

    fn modify_number(&mut self, id: u32, value: &validate::Number, op: Op) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match (&property, value) {
                (Property { payload: Payload::Int(old), .. }, Number::IntT(value)) => {
                    property.payload = Payload::Int((op.int)(*old, *value));
                }
                (Property { payload: Payload::Uint(old), .. }, Number::UintT(value)) => {
                    property.payload = Payload::Uint((op.uint)(*old, *value));
                }
                (Property { payload: Payload::Double(old), .. }, Number::DoubleT(value)) => {
                    property.payload = Payload::Double((op.double)(*old, *value));
                }
                unexpected => {
                    return Err(format_err!("Bad types {:?} trying to set number", unexpected))
                }
            }
        } else {
            return Err(format_err!("Tried to {} number on nonexistent property {}", op.name, id));
        }
        Ok(())
    }

    fn check_index<T>(numbers: &Vec<T>, index: usize) -> Result<(), Error> {
        if index >= numbers.len() as usize {
            return Err(format_err!("Index {} too big for vector length {}", index, numbers.len()));
        }
        Ok(())
    }

    fn modify_array(
        &mut self,
        id: u32,
        index64: u64,
        value: &validate::Number,
        op: Op,
    ) -> Result<(), Error> {
        if let Some(mut property) = self.properties.get_mut(&id) {
            let index = index64 as usize;
            // Out of range index is a NOP, not an error.
            let number_len = match &property {
                Property { payload: Payload::IntArray(numbers, ArrayFormat::Default), .. } => {
                    numbers.len()
                }
                Property { payload: Payload::UintArray(numbers, ArrayFormat::Default), .. } => {
                    numbers.len()
                }
                Property {
                    payload: Payload::DoubleArray(numbers, ArrayFormat::Default), ..
                } => numbers.len(),
                unexpected => {
                    return Err(format_err!("Bad types {:?} trying to set number", unexpected))
                }
            };
            if index >= number_len {
                return Ok(());
            }
            match (&mut property, value) {
                (Property { payload: Payload::IntArray(numbers, _), .. }, Number::IntT(value)) => {
                    Self::check_index(numbers, index)?;
                    numbers[index] = (op.int)(numbers[index], *value);
                }
                (
                    Property { payload: Payload::UintArray(numbers, _), .. },
                    Number::UintT(value),
                ) => {
                    Self::check_index(numbers, index)?;
                    numbers[index] = (op.uint)(numbers[index], *value);
                }
                (
                    Property { payload: Payload::DoubleArray(numbers, _), .. },
                    Number::DoubleT(value),
                ) => {
                    Self::check_index(numbers, index)?;
                    numbers[index] = (op.double)(numbers[index], *value);
                }
                unexpected => {
                    return Err(format_err!("Type mismatch {:?} trying to set number", unexpected))
                }
            }
        } else {
            return Err(format_err!("Tried to {} number on nonexistent property {}", op.name, id));
        }
        Ok(())
    }

    fn set_string(&mut self, id: u32, value: &String) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match &property {
                Property { payload: Payload::String(_), .. } => {
                    property.payload = Payload::String(value.to_owned())
                }
                unexpected => {
                    return Err(format_err!("Bad type {:?} trying to set string", unexpected))
                }
            }
        } else {
            return Err(format_err!("Tried to set string on nonexistent property {}", id));
        }
        Ok(())
    }

    fn set_bytes(&mut self, id: u32, value: &Vec<u8>) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match &property {
                Property { payload: Payload::Bytes(_), .. } => {
                    property.payload = Payload::Bytes(value.to_owned())
                }
                unexpected => {
                    return Err(format_err!("Bad type {:?} trying to set bytes", unexpected))
                }
            }
        } else {
            return Err(format_err!("Tried to set bytes on nonexistent property {}", id));
        }
        Ok(())
    }

    fn set_bool(&mut self, id: u32, value: bool) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match &property {
                Property { payload: Payload::Bool(_), .. } => {
                    property.payload = Payload::Bool(value)
                }
                unexpected => {
                    return Err(format_err!("Bad type {:?} trying to set bool", unexpected))
                }
            }
        } else {
            return Err(format_err!("Tried to set bool on nonexistent property {}", id));
        }
        Ok(())
    }

    fn add_lazy_node(
        &mut self,
        parent: u32,
        id: u32,
        name: &str,
        disposition: &validate::LinkDisposition,
        actions: &Vec<validate::Action>,
    ) -> Result<(), Error> {
        let mut parsed_data = Data::new();
        parsed_data.apply_multiple(&actions)?;
        self.add_property(
            parent,
            id,
            name,
            Payload::Link {
                disposition: match disposition {
                    validate::LinkDisposition::Child => LinkNodeDisposition::Child,
                    validate::LinkDisposition::Inline => LinkNodeDisposition::Inline,
                },
                parsed_data,
            },
        )?;
        Ok(())
    }

    fn modify_lazy_node(&mut self, id: u32, actions: &Vec<validate::Action>) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match &property {
                Property { payload: Payload::Link { disposition, .. }, .. } => {
                    property.payload = Payload::Link {
                        disposition: disposition.clone(),
                        parsed_data: {
                            let mut parsed_data = Data::new();
                            parsed_data.apply_multiple(actions)?;
                            parsed_data
                        },
                    }
                }
                unexpected => {
                    return Err(format_err!("Bad type {:?} trying to update lazy node", unexpected))
                }
            }
        } else {
            return Err(format_err!("Tried to update non-existent lazy node {}.", id));
        }
        Ok(())
    }

    // ***** Here are the functions to compare two Data (by converting to a
    // ***** fully informative string).

    fn diff_string(string1: &str, string2: &str) -> String {
        let difference::Changeset { diffs, .. } =
            difference::Changeset::new(string1, string2, "\n");
        let mut strings = Vec::new();
        for diff in diffs.iter() {
            match diff {
                difference::Difference::Same(lines) => strings.push(format!(
                    "\nSame:{} lines",
                    lines.split("\n").collect::<Vec<&str>>().len()
                )),
                difference::Difference::Rem(lines) => strings.push(format!("\nLocal: {}", lines)),
                difference::Difference::Add(lines) => strings.push(format!("\nOther: {}", lines)),
            }
        }
        strings.push("\n".to_owned());
        strings.join("")
    }

    /// Compares two in-memory Inspect trees, returning Ok(()) if they have the
    /// same data and an Err<> if they are different. The string in the Err<>
    /// may be very large.
    pub fn compare(&self, other: &Data, diff_type: DiffType) -> Result<(), Error> {
        let self_string = self.to_string();
        let other_string = other.to_string();
        if self_string == other_string {
            Ok(())
        } else {
            let full_string = match diff_type {
                DiffType::Diff => "".to_owned(),
                DiffType::Full | DiffType::Both => {
                    format!("-- LOCAL --\n{}\n-- OTHER --\n{}\n", self_string, other_string)
                }
            };
            let diff_string = match diff_type {
                DiffType::Full => "".to_owned(),
                DiffType::Diff | DiffType::Both => {
                    format!("-- DIFF --\n{}\n", Self::diff_string(&self_string, &other_string))
                }
            };
            return Err(format_err!("Trees differ:{}{}", full_string, diff_string));
        }
    }

    /// Generates a string fully representing the Inspect data.
    pub fn to_string(&self) -> String {
        self.to_string_internal(false)
    }

    /// This creates a new Data. Note that the standard "root" node of the VMO API
    /// corresponds to the index-0 node added here.
    pub fn new() -> Data {
        let mut ret = Data {
            nodes: HashMap::new(),
            properties: HashMap::new(),
            tombstone_nodes: HashSet::new(),
            tombstone_properties: HashSet::new(),
        };
        ret.nodes.insert(
            0,
            Node {
                name: ROOT_NAME.into(),
                parent: 0,
                children: HashSet::new(),
                properties: HashSet::new(),
            },
        );
        ret
    }

    fn build(nodes: HashMap<u32, Node>, properties: HashMap<u32, Property>) -> Data {
        Data {
            nodes,
            properties,
            tombstone_nodes: HashSet::new(),
            tombstone_properties: HashSet::new(),
        }
    }

    fn to_string_internal(&self, hide_root: bool) -> String {
        if let Some(node) = self.nodes.get(&ROOT_ID) {
            node.to_string(&"".to_owned(), self, hide_root)
        } else {
            "No root node; internal error\n".to_owned()
        }
    }

    fn apply_multiple(&mut self, actions: &Vec<validate::Action>) -> Result<(), Error> {
        for action in actions {
            self.apply(&action)?;
        }
        Ok(())
    }
}

// There's no enum in fuchsia_inspect::format::block which contains only
// values that are valid for an ArrayType.
#[derive(Debug, PartialEq, Eq, FromPrimitive, ToPrimitive)]
enum ArrayType {
    Int = 4,
    Uint = 5,
    Double = 6,
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::*,
        fidl_test_inspect_validate::{Number, NumberType, ROOT_ID},
        fuchsia_inspect::format::block_type::BlockType,
    };

    #[test]
    fn test_basic_data_strings() -> Result<(), Error> {
        let mut info = Data::new();
        assert_eq!(info.to_string(), " root ->\n\n\n");
        info.apply(&create_node!(parent: ROOT_ID, id: 1, name: "foo"))?;
        assert_eq!(info.to_string(), " root ->\n\n>  foo ->\n\n\n\n");
        info.apply(&delete_node!( id: 1 ))?;
        assert_eq!(info.to_string(), " root ->\n\n\n");
        Ok(())
    }

    // Make sure every action correctly modifies the string representation of the data tree.
    #[test]
    fn test_creation_deletion() -> Result<(), Error> {
        let mut info = Data::new();
        assert!(!info.to_string().contains("child ->"));
        info.apply(&create_node!(parent: ROOT_ID, id: 1, name: "child"))?;
        assert!(info.to_string().contains("child ->"));
        info.apply(&create_node!(parent: 1, id: 2, name: "grandchild"))?;
        assert!(
            info.to_string().contains("grandchild ->") && info.to_string().contains("child ->")
        );
        info.apply(
            &create_numeric_property!(parent: ROOT_ID, id: 3, name: "int-42", value: Number::IntT(-42)),
        )?;
        assert!(info.to_string().contains("int-42: Int(-42)")); // Make sure it can hold negative #
        info.apply(&create_string_property!(parent: 1, id: 4, name: "stringfoo", value: "foo"))?;
        assert_eq!(
            info.to_string(),
            " root ->\n>  int-42: Int(-42)\n>  child ->\
             \n> >  stringfoo: String(\"foo\")\n> >  grandchild ->\n\n\n\n\n"
        );
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 5, name: "uint", value: Number::UintT(1024)))?;
        assert!(info.to_string().contains("uint: Uint(1024)"));
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 6, name: "frac", value: Number::DoubleT(0.5)))?;
        assert!(info.to_string().contains("frac: Double(0.5)"));
        info.apply(
            &create_bytes_property!(parent: ROOT_ID, id: 7, name: "bytes", value: vec!(1u8, 2u8)),
        )?;
        assert!(info.to_string().contains("bytes: Bytes([1, 2])"));
        info.apply(&create_array_property!(parent: ROOT_ID, id: 8, name: "i_ntarr", slots: 1, type: NumberType::Int))?;
        assert!(info.to_string().contains("i_ntarr: IntArray([0], Default)"));
        info.apply(&create_array_property!(parent: ROOT_ID, id: 9, name: "u_intarr", slots: 2, type: NumberType::Uint))?;
        assert!(info.to_string().contains("u_intarr: UintArray([0, 0], Default)"));
        info.apply(&create_array_property!(parent: ROOT_ID, id: 10, name: "dblarr", slots: 3, type: NumberType::Double))?;
        assert!(info.to_string().contains("dblarr: DoubleArray([0.0, 0.0, 0.0], Default)"));
        info.apply(&create_linear_histogram!(parent: ROOT_ID, id: 11, name: "ILhist", floor: 12,
            step_size: 3, buckets: 2, type: IntT))?;
        assert!(info
            .to_string()
            .contains("ILhist: IntArray([12, 3, 0, 0, 0, 0], LinearHistogram)"));
        info.apply(&create_linear_histogram!(parent: ROOT_ID, id: 12, name: "ULhist", floor: 34,
            step_size: 5, buckets: 2, type: UintT))?;
        assert!(info
            .to_string()
            .contains("ULhist: UintArray([34, 5, 0, 0, 0, 0], LinearHistogram)"));
        info.apply(
            &create_linear_histogram!(parent: ROOT_ID, id: 13, name: "DLhist", floor: 56.0,
            step_size: 7.0, buckets: 2, type: DoubleT),
        )?;
        assert!(info
            .to_string()
            .contains("DLhist: DoubleArray([56.0, 7.0, 0.0, 0.0, 0.0, 0.0], LinearHistogram)"));
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 14, name: "IEhist",
            floor: 12, initial_step: 3, step_multiplier: 5, buckets: 2, type: IntT))?;
        assert!(info
            .to_string()
            .contains("IEhist: IntArray([12, 3, 5, 0, 0, 0, 0], ExponentialHistogram)"));
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 15, name: "UEhist",
            floor: 34, initial_step: 9, step_multiplier: 6, buckets: 2, type: UintT))?;
        assert!(info
            .to_string()
            .contains("UEhist: UintArray([34, 9, 6, 0, 0, 0, 0], ExponentialHistogram)"));
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 16, name: "DEhist",
            floor: 56.0, initial_step: 27.0, step_multiplier: 7.0, buckets: 2, type: DoubleT))?;
        assert!(info.to_string().contains(
            "DEhist: DoubleArray([56.0, 27.0, 7.0, 0.0, 0.0, 0.0, 0.0], ExponentialHistogram)"
        ));
        info.apply(&create_bool_property!(parent: ROOT_ID, id: 17, name: "bool", value: true))?;
        assert!(info.to_string().contains("bool: Bool(true)"));

        info.apply(&delete_property!(id: 3))?;
        assert!(!info.to_string().contains("int-42") && info.to_string().contains("stringfoo"));
        info.apply(&delete_property!(id: 4))?;
        assert!(!info.to_string().contains("stringfoo"));
        info.apply(&delete_property!(id: 5))?;
        assert!(!info.to_string().contains("uint"));
        info.apply(&delete_property!(id: 6))?;
        assert!(!info.to_string().contains("frac"));
        info.apply(&delete_property!(id: 7))?;
        assert!(!info.to_string().contains("bytes"));
        info.apply(&delete_property!(id: 8))?;
        assert!(!info.to_string().contains("i_ntarr"));
        info.apply(&delete_property!(id: 9))?;
        assert!(!info.to_string().contains("u_intarr"));
        info.apply(&delete_property!(id: 10))?;
        assert!(!info.to_string().contains("dblarr"));
        info.apply(&delete_property!(id: 11))?;
        assert!(!info.to_string().contains("ILhist"));
        info.apply(&delete_property!(id: 12))?;
        assert!(!info.to_string().contains("ULhist"));
        info.apply(&delete_property!(id: 13))?;
        assert!(!info.to_string().contains("DLhist"));
        info.apply(&delete_property!(id: 14))?;
        assert!(!info.to_string().contains("IEhist"));
        info.apply(&delete_property!(id: 15))?;
        assert!(!info.to_string().contains("UEhist"));
        info.apply(&delete_property!(id: 16))?;
        assert!(!info.to_string().contains("DEhist"));
        info.apply(&delete_property!(id: 17))?;
        assert!(!info.to_string().contains("bool"));
        info.apply(&delete_node!(id:2))?;
        assert!(!info.to_string().contains("grandchild") && info.to_string().contains("child"));
        info.apply(&delete_node!( id: 1 ))?;
        assert_eq!(info.to_string(), " root ->\n\n\n");
        Ok(())
    }

    #[test]
    fn test_basic_int_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 3, name: "value",
                                     value: Number::IntT(-42)))?;
        assert!(info.apply(&add_number!(id: 3, value: Number::IntT(3))).is_ok());
        assert!(info.to_string().contains("value: Int(-39)"));
        assert!(info.apply(&add_number!(id: 3, value: Number::UintT(3))).is_err());
        assert!(info.apply(&add_number!(id: 3, value: Number::DoubleT(3.0))).is_err());
        assert!(info.to_string().contains("value: Int(-39)"));
        assert!(info.apply(&subtract_number!(id: 3, value: Number::IntT(5))).is_ok());
        assert!(info.to_string().contains("value: Int(-44)"));
        assert!(info.apply(&subtract_number!(id: 3, value: Number::UintT(5))).is_err());
        assert!(info.apply(&subtract_number!(id: 3, value: Number::DoubleT(5.0))).is_err());
        assert!(info.to_string().contains("value: Int(-44)"));
        assert!(info.apply(&set_number!(id: 3, value: Number::IntT(22))).is_ok());
        assert!(info.to_string().contains("value: Int(22)"));
        assert!(info.apply(&set_number!(id: 3, value: Number::UintT(23))).is_err());
        assert!(info.apply(&set_number!(id: 3, value: Number::DoubleT(24.0))).is_err());
        assert!(info.to_string().contains("value: Int(22)"));
        Ok(())
    }

    #[test]
    fn test_array_int_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_array_property!(parent: ROOT_ID, id: 3, name: "value", slots: 3,
                                           type: NumberType::Int))?;
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::IntT(3))).is_ok());
        assert!(info.to_string().contains("value: IntArray([0, 3, 0], Default)"));
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::UintT(3))).is_err());
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::DoubleT(3.0))).is_err());
        assert!(info.to_string().contains("value: IntArray([0, 3, 0], Default)"));
        assert!(info.apply(&array_subtract!(id: 3, index: 2, value: Number::IntT(5))).is_ok());
        assert!(info.to_string().contains("value: IntArray([0, 3, -5], Default)"));
        assert!(info.apply(&array_subtract!(id: 3, index: 2, value: Number::UintT(5))).is_err());
        assert!(info
            .apply(&array_subtract!(id: 3, index: 2,
                                            value: Number::DoubleT(5.0)))
            .is_err());
        assert!(info.to_string().contains("value: IntArray([0, 3, -5], Default)"));
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::IntT(22))).is_ok());
        assert!(info.to_string().contains("value: IntArray([0, 22, -5], Default)"));
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::UintT(23))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::DoubleT(24.0))).is_err());
        assert!(info.to_string().contains("value: IntArray([0, 22, -5], Default)"));
        Ok(())
    }

    #[test]
    fn test_linear_int_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_linear_histogram!(parent: ROOT_ID, id: 3, name: "value",
                    floor: 4, step_size: 2, buckets: 2, type: IntT))?;
        assert!(info.to_string().contains("value: IntArray([4, 2, 0, 0, 0, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(4))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 0, 1, 0, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(5))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 0, 2, 0, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(6))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 0, 2, 1, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(8))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 0, 2, 1, 1], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(std::i64::MAX))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 0, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(0))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 1, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(std::i64::MIN))).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 2, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(0))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(0.0))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::IntT(222))).is_err());
        assert!(info.to_string().contains("value: IntArray([4, 2, 2, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert_multiple!(id: 3, value: Number::IntT(7), count: 4)).is_ok());
        assert!(info.to_string().contains("value: IntArray([4, 2, 2, 2, 5, 2], LinearHistogram)"));
        Ok(())
    }

    #[test]
    fn test_exponential_int_ops() -> Result<(), Error> {
        let mut info = Data::new();
        // Bucket boundaries are 5, 7, 13
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 3, name: "value",
                    floor: 5, initial_step: 2,
                    step_multiplier: 4, buckets: 2, type: IntT))?;
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 0, 0, 0, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(5))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 0, 1, 0, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(6))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 0, 2, 0, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(7))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 0, 2, 1, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(13))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 0, 2, 1, 1], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(std::i64::MAX))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 0, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(0))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 1, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(std::i64::MIN))).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 2, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(0))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(0.0))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::IntT(222))).is_err());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 2, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert_multiple!(id: 3, value: Number::IntT(12), count: 4)).is_ok());
        assert!(info
            .to_string()
            .contains("value: IntArray([5, 2, 4, 2, 2, 5, 2], ExponentialHistogram)"));
        Ok(())
    }

    #[test]
    fn test_array_out_of_bounds_nop() -> Result<(), Error> {
        // Accesses to indexes beyond the array are legal and should have no effect on the data.
        let mut info = Data::new();
        info.apply(&create_array_property!(parent: ROOT_ID, id: 3, name: "value", slots: 3,
                                           type: NumberType::Int))?;
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::IntT(3))).is_ok());
        assert!(info.to_string().contains("value: IntArray([0, 3, 0], Default)"));
        assert!(info.apply(&array_add!(id: 3, index: 3, value: Number::IntT(3))).is_ok());
        assert!(info.apply(&array_add!(id: 3, index: 6, value: Number::IntT(3))).is_ok());
        assert!(info.apply(&array_add!(id: 3, index: 12345, value: Number::IntT(3))).is_ok());
        assert!(info.to_string().contains("value: IntArray([0, 3, 0], Default)"));
        Ok(())
    }

    #[test]
    fn test_basic_uint_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 3, name: "value",
                                     value: Number::UintT(42)))?;
        assert!(info.apply(&add_number!(id: 3, value: Number::UintT(3))).is_ok());
        assert!(info.to_string().contains("value: Uint(45)"));
        assert!(info.apply(&add_number!(id: 3, value: Number::IntT(3))).is_err());
        assert!(info.apply(&add_number!(id: 3, value: Number::DoubleT(3.0))).is_err());
        assert!(info.to_string().contains("value: Uint(45)"));
        assert!(info.apply(&subtract_number!(id: 3, value: Number::UintT(5))).is_ok());
        assert!(info.to_string().contains("value: Uint(40)"));
        assert!(info.apply(&subtract_number!(id: 3, value: Number::IntT(5))).is_err());
        assert!(info.apply(&subtract_number!(id: 3, value: Number::DoubleT(5.0))).is_err());
        assert!(info.to_string().contains("value: Uint(40)"));
        assert!(info.apply(&set_number!(id: 3, value: Number::UintT(22))).is_ok());
        assert!(info.to_string().contains("value: Uint(22)"));
        assert!(info.apply(&set_number!(id: 3, value: Number::IntT(23))).is_err());
        assert!(info.apply(&set_number!(id: 3, value: Number::DoubleT(24.0))).is_err());
        assert!(info.to_string().contains("value: Uint(22)"));
        Ok(())
    }

    #[test]
    fn test_array_uint_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_array_property!(parent: ROOT_ID, id: 3, name: "value", slots: 3,
                                     type: NumberType::Uint))?;
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::UintT(3))).is_ok());
        assert!(info.to_string().contains("value: UintArray([0, 3, 0], Default)"));
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::IntT(3))).is_err());
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::DoubleT(3.0))).is_err());
        assert!(info.to_string().contains("value: UintArray([0, 3, 0], Default)"));
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::UintT(22))).is_ok());
        assert!(info.to_string().contains("value: UintArray([0, 22, 0], Default)"));
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::IntT(23))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::DoubleT(24.0))).is_err());
        assert!(info.to_string().contains("value: UintArray([0, 22, 0], Default)"));
        assert!(info.apply(&array_subtract!(id: 3, index: 1, value: Number::UintT(5))).is_ok());
        assert!(info.to_string().contains("value: UintArray([0, 17, 0], Default)"));
        assert!(info.apply(&array_subtract!(id: 3, index: 1, value: Number::IntT(5))).is_err());
        assert!(info
            .apply(&array_subtract!(id: 3, index: 1, value: Number::DoubleT(5.0)))
            .is_err());
        assert!(info.to_string().contains("value: UintArray([0, 17, 0], Default)"));
        Ok(())
    }

    #[test]
    fn test_linear_uint_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_linear_histogram!(parent: ROOT_ID, id: 3, name: "value",
                    floor: 4, step_size: 2, buckets: 2, type: UintT))?;
        assert!(info.to_string().contains("value: UintArray([4, 2, 0, 0, 0, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(4))).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 0, 1, 0, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(5))).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 0, 2, 0, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(6))).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 0, 2, 1, 0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(8))).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 0, 2, 1, 1], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(std::u64::MAX))).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 0, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(0))).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 1, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(0))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(0.0))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::UintT(222))).is_err());
        assert!(info.to_string().contains("value: UintArray([4, 2, 1, 2, 1, 2], LinearHistogram)"));
        assert!(info.apply(&insert_multiple!(id: 3, value: Number::UintT(7), count: 4)).is_ok());
        assert!(info.to_string().contains("value: UintArray([4, 2, 1, 2, 5, 2], LinearHistogram)"));
        Ok(())
    }

    #[test]
    fn test_exponential_uint_ops() -> Result<(), Error> {
        let mut info = Data::new();
        // Bucket boundaries are 5, 7, 13
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 3, name: "value",
                    floor: 5, initial_step: 2,
                    step_multiplier: 4, buckets: 2, type: UintT))?;
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 0, 0, 0, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(5))).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 0, 1, 0, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(6))).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 0, 2, 0, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(7))).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 0, 2, 1, 0], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(13))).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 0, 2, 1, 1], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(std::u64::MAX))).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 0, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(0))).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 1, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(0))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(0.0))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::UintT(222))).is_err());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 1, 2, 1, 2], ExponentialHistogram)"));
        assert!(info.apply(&insert_multiple!(id: 3, value: Number::UintT(12), count: 4)).is_ok());
        assert!(info
            .to_string()
            .contains("value: UintArray([5, 2, 4, 1, 2, 5, 2], ExponentialHistogram)"));
        Ok(())
    }

    #[test]
    fn test_basic_double_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 3, name: "value",
                                     value: Number::DoubleT(42.0)))?;
        assert!(info.apply(&add_number!(id: 3, value: Number::DoubleT(3.0))).is_ok());
        assert!(info.to_string().contains("value: Double(45.0)"));
        assert!(info.apply(&add_number!(id: 3, value: Number::IntT(3))).is_err());
        assert!(info.apply(&add_number!(id: 3, value: Number::UintT(3))).is_err());
        assert!(info.to_string().contains("value: Double(45.0)"));
        assert!(info.apply(&subtract_number!(id: 3, value: Number::DoubleT(5.0))).is_ok());
        assert!(info.to_string().contains("value: Double(40.0)"));
        assert!(info.apply(&subtract_number!(id: 3, value: Number::UintT(5))).is_err());
        assert!(info.apply(&subtract_number!(id: 3, value: Number::UintT(5))).is_err());
        assert!(info.to_string().contains("value: Double(40.0)"));
        assert!(info.apply(&set_number!(id: 3, value: Number::DoubleT(22.0))).is_ok());
        assert!(info.to_string().contains("value: Double(22.0)"));
        assert!(info.apply(&set_number!(id: 3, value: Number::UintT(23))).is_err());
        assert!(info.apply(&set_number!(id: 3, value: Number::UintT(24))).is_err());
        assert!(info.to_string().contains("value: Double(22.0)"));
        Ok(())
    }

    #[test]
    fn test_array_double_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_array_property!(parent: ROOT_ID, id: 3, name: "value", slots: 3,
                                     type: NumberType::Double))?;
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::DoubleT(3.0))).is_ok());
        assert!(info.to_string().contains("value: DoubleArray([0.0, 3.0, 0.0], Default)"));
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::IntT(3))).is_err());
        assert!(info.apply(&array_add!(id: 3, index: 1, value: Number::UintT(3))).is_err());
        assert!(info.to_string().contains("value: DoubleArray([0.0, 3.0, 0.0], Default)"));
        assert!(info.apply(&array_subtract!(id: 3, index: 2, value: Number::DoubleT(5.0))).is_ok());
        assert!(info.to_string().contains("value: DoubleArray([0.0, 3.0, -5.0], Default)"));
        assert!(info.apply(&array_subtract!(id: 3, index: 2, value: Number::IntT(5))).is_err());
        assert!(info.apply(&array_subtract!(id: 3, index: 2, value: Number::UintT(5))).is_err());
        assert!(info.to_string().contains("value: DoubleArray([0.0, 3.0, -5.0], Default)"));
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::DoubleT(22.0))).is_ok());
        assert!(info.to_string().contains("value: DoubleArray([0.0, 22.0, -5.0], Default)"));
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::IntT(23))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::IntT(24))).is_err());
        assert!(info.to_string().contains("value: DoubleArray([0.0, 22.0, -5.0], Default)"));
        Ok(())
    }

    #[test]
    fn test_linear_double_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_linear_histogram!(parent: ROOT_ID, id: 3, name: "value",
                    floor: 4.0, step_size: 0.5, buckets: 2, type: DoubleT))?;
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 0.0, 0.0, 0.0, 0.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(4.0))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 0.0, 1.0, 0.0, 0.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(4.25))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 0.0, 2.0, 0.0, 0.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(4.75))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 0.0, 2.0, 1.0, 0.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(5.1))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 0.0, 2.0, 1.0, 1.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(std::f64::MAX))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 0.0, 2.0, 1.0, 2.0], LinearHistogram)"));
        assert!(info
            .apply(&insert!(id: 3, value: Number::DoubleT(std::f64::MIN_POSITIVE)))
            .is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 1.0, 2.0, 1.0, 2.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(std::f64::MIN))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 2.0, 2.0, 1.0, 2.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(0.0))).is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 3.0, 2.0, 1.0, 2.0], LinearHistogram)"));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(0))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(0))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::DoubleT(222.0))).is_err());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 3.0, 2.0, 1.0, 2.0], LinearHistogram)"));
        assert!(info
            .apply(&insert_multiple!(id: 3, value: Number::DoubleT(4.5), count: 4))
            .is_ok());
        assert!(info
            .to_string()
            .contains("value: DoubleArray([4.0, 0.5, 3.0, 2.0, 5.0, 2.0], LinearHistogram)"));
        Ok(())
    }

    #[test]
    fn test_exponential_double_ops() -> Result<(), Error> {
        let mut info = Data::new();
        // Bucket boundaries are 5, 7, 13, 37
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 3, name: "value",
                    floor: 5.0, initial_step: 2.0,
                    step_multiplier: 4.0, buckets: 3, type: DoubleT))?;
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 0.0, 0.0, 0.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(5.0))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 1.0, 0.0, 0.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(6.9))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 0.0, 0.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(7.1))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 1.0, 0.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info
            .apply(&insert_multiple!(id: 3, value: Number::DoubleT(12.9), count: 4))
            .is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 5.0, 0.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(13.1))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 5.0, 1.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(36.9))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 5.0, 2.0, 0.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(37.1))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 5.0, 2.0, 1.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(std::f64::MAX))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 0.0, 2.0, 5.0, 2.0, 2.0], ExponentialHistogram)"
        ));
        assert!(info
            .apply(&insert!(id: 3, value: Number::DoubleT(std::f64::MIN_POSITIVE)))
            .is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 1.0, 2.0, 5.0, 2.0, 2.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(std::f64::MIN))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 2.0, 2.0, 5.0, 2.0, 2.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::DoubleT(0.0))).is_ok());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 3.0, 2.0, 5.0, 2.0, 2.0], ExponentialHistogram)"
        ));
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(0))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::UintT(0))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 1, value: Number::DoubleT(222.0))).is_err());
        assert!(info.to_string().contains(
            "value: DoubleArray([5.0, 2.0, 4.0, 3.0, 2.0, 5.0, 2.0, 2.0], ExponentialHistogram)"
        ));
        Ok(())
    }

    #[test]
    fn test_basic_vector_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply(&create_string_property!(parent: ROOT_ID, id: 3, name: "value",
                                     value: "foo"))?;
        assert!(info.to_string().contains("value: String(\"foo\")"));
        assert!(info.apply(&set_string!(id: 3, value: "bar")).is_ok());
        assert!(info.to_string().contains("value: String(\"bar\")"));
        assert!(info.apply(&set_bytes!(id: 3, value: vec!(3u8))).is_err());
        assert!(info.to_string().contains("value: String(\"bar\")"));
        info.apply(&create_bytes_property!(parent: ROOT_ID, id: 4, name: "bvalue",
                                     value: vec!(1u8, 2u8)))?;
        assert!(info.to_string().contains("bvalue: Bytes([1, 2])"));
        assert!(info.apply(&set_bytes!(id: 4, value: vec!(3u8, 4u8))).is_ok());
        assert!(info.to_string().contains("bvalue: Bytes([3, 4])"));
        assert!(info.apply(&set_string!(id: 4, value: "baz")).is_err());
        assert!(info.to_string().contains("bvalue: Bytes([3, 4])"));
        Ok(())
    }

    #[test]
    fn test_basic_lazy_node_ops() -> Result<(), Error> {
        let mut info = Data::new();
        info.apply_lazy(&create_lazy_node!(parent: ROOT_ID, id: 1, name: "child", disposition: validate::LinkDisposition::Child, actions: vec![create_bytes_property!(parent: ROOT_ID, id: 1, name: "child_bytes",value: vec!(3u8, 4u8))]))?;
        info.apply_lazy(&create_lazy_node!(parent: ROOT_ID, id: 2, name: "inline", disposition: validate::LinkDisposition::Inline, actions: vec![create_bytes_property!(parent: ROOT_ID, id: 1, name: "inline_bytes",value: vec!(3u8, 4u8))]))?;

        // Outputs 'Inline' and 'Child' dispositions differently.
        assert_eq!(
            info.to_string(),
            " root ->\n>  child: >  child_bytes: Bytes([3, 4])\n\n\n>  inline_bytes: Bytes([3, 4])\n\n\n\n"
        );

        info.apply_lazy(&delete_lazy_node!(id: 1))?;
        // Outputs only 'Inline' lazy node since 'Child' lazy node was deleted
        assert_eq!(info.to_string(), " root ->\n>  inline_bytes: Bytes([3, 4])\n\n\n\n");

        info.apply_lazy(&modify_lazy_node!(id: 2, actions: vec![create_bytes_property!(parent: ROOT_ID, id: 1, name: "inline_bytes_modified",value: vec!(4u8, 2u8))]))?;
        // Outputs modfied lazy node.
        assert_eq!(info.to_string(), " root ->\n>  inline_bytes_modified: Bytes([4, 2])\n\n\n\n");

        Ok(())
    }

    #[test]
    fn test_illegal_node_actions() -> Result<(), Error> {
        let mut info = Data::new();
        // Parent must exist
        assert!(info.apply(&create_node!(parent: 42, id: 1, name: "child")).is_err());
        // Can't reuse node IDs
        info = Data::new();
        info.apply(&create_node!(parent: ROOT_ID, id: 1, name: "child"))?;
        assert!(info.apply(&create_node!(parent: ROOT_ID, id: 1, name: "another_child")).is_err());
        // Can't delete root
        info = Data::new();
        assert!(info.apply(&delete_node!(id: ROOT_ID)).is_err());
        // Can't delete nonexistent node
        info = Data::new();
        assert!(info.apply(&delete_node!(id: 333)).is_err());
        Ok(())
    }

    #[test]
    fn test_illegal_property_actions() -> Result<(), Error> {
        let mut info = Data::new();
        // Parent must exist
        assert!(info
            .apply(
                &create_numeric_property!(parent: 42, id: 1, name: "answer", value: Number::IntT(42))
            )
            .is_err());
        // Can't reuse property IDs
        info = Data::new();
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 1, name: "answer", value: Number::IntT(42)))?;
        assert!(info
            .apply(&create_numeric_property!(parent: ROOT_ID, id: 1, name: "another_answer", value: Number::IntT(7)))
            .is_err());
        // Can't delete nonexistent property
        info = Data::new();
        assert!(info.apply(&delete_property!(id: 1)).is_err());
        // Can't do basic-int on array or histogram, or any vice versa
        info = Data::new();
        info.apply(&create_numeric_property!(parent: ROOT_ID, id: 3, name: "value",
                                     value: Number::IntT(42)))?;
        info.apply(&create_array_property!(parent: ROOT_ID, id: 4, name: "array", slots: 2,
                                     type: NumberType::Int))?;
        info.apply(&create_linear_histogram!(parent: ROOT_ID, id: 5, name: "lin",
                                floor: 5, step_size: 2,
                                buckets: 2, type: IntT))?;
        info.apply(&create_exponential_histogram!(parent: ROOT_ID, id: 6, name: "exp",
                                floor: 5, initial_step: 2,
                                step_multiplier: 2, buckets: 2, type: IntT))?;
        assert!(info.apply(&set_number!(id: 3, value: Number::IntT(5))).is_ok());
        assert!(info.apply(&array_set!(id: 4, index: 0, value: Number::IntT(5))).is_ok());
        assert!(info.apply(&insert!(id: 5, value: Number::IntT(2))).is_ok());
        assert!(info.apply(&insert!(id: 6, value: Number::IntT(2))).is_ok());
        assert!(info.apply(&insert_multiple!(id: 5, value: Number::IntT(2), count: 3)).is_ok());
        assert!(info.apply(&insert_multiple!(id: 6, value: Number::IntT(2), count: 3)).is_ok());
        assert!(info.apply(&set_number!(id: 4, value: Number::IntT(5))).is_err());
        assert!(info.apply(&set_number!(id: 5, value: Number::IntT(5))).is_err());
        assert!(info.apply(&set_number!(id: 6, value: Number::IntT(5))).is_err());
        assert!(info.apply(&array_set!(id: 3, index: 0, value: Number::IntT(5))).is_err());
        assert!(info.apply(&array_set!(id: 5, index: 0, value: Number::IntT(5))).is_err());
        assert!(info.apply(&array_set!(id: 6, index: 0, value: Number::IntT(5))).is_err());
        assert!(info.apply(&insert!(id: 3, value: Number::IntT(2))).is_err());
        assert!(info.apply(&insert!(id: 4, value: Number::IntT(2))).is_err());
        assert!(info.apply(&insert_multiple!(id: 3, value: Number::IntT(2), count: 3)).is_err());
        assert!(info.apply(&insert_multiple!(id: 4, value: Number::IntT(2), count: 3)).is_err());
        Ok(())
    }

    #[test]
    fn test_enum_values() {
        assert_eq!(BlockType::IntValue.to_isize().unwrap(), ArrayType::Int.to_isize().unwrap());
        assert_eq!(BlockType::UintValue.to_isize().unwrap(), ArrayType::Uint.to_isize().unwrap());
        assert_eq!(
            BlockType::DoubleValue.to_isize().unwrap(),
            ArrayType::Double.to_isize().unwrap()
        );
    }

    #[test]
    fn test_create_node_checks() {
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data.apply(&create_node!(parent: 0, id: 2, name: "second")).is_ok());
        assert!(data.apply(&create_node!(parent: 1, id: 3, name: "child")).is_ok());
        assert!(data.apply(&create_node!(parent: 0, id: 2, name: "double")).is_err());
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 1, id: 2, name: "orphan")).is_err());
    }

    #[test]
    fn test_delete_node_checks() {
        let mut data = Data::new();
        assert!(data.apply(&delete_node!(id: 0)).is_err());
        let mut data = Data::new();
        data.apply(&create_node!(parent: 0, id: 1, name: "first")).ok();
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_err());
    }

    #[test]
    // Make sure tombstoning works correctly (tracking implicitly deleted descendants).
    fn test_node_tombstoning() {
        // Can delete, but not double-delete, a tombstoned node.
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data
            .apply(&create_numeric_property!(parent: 1, id: 2,
            name: "answer", value: Number::IntT(42)))
            .is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data.apply(&delete_property!(id: 2)).is_ok());
        assert!(data.apply(&delete_property!(id: 2)).is_err());
        // Can tombstone, then delete, then create.
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data
            .apply(&create_numeric_property!(parent: 1, id: 2,
            name: "answer", value: Number::IntT(42)))
            .is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data.apply(&delete_property!(id: 2)).is_ok());
        assert!(data
            .apply(&create_numeric_property!(parent: 0, id: 2,
            name: "root_answer", value: Number::IntT(42)))
            .is_ok());
        // Cannot tombstone, then create.
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data
            .apply(&create_numeric_property!(parent: 1, id: 2,
            name: "answer", value: Number::IntT(42)))
            .is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data
            .apply(&create_numeric_property!(parent: 0, id: 2,
            name: "root_answer", value: Number::IntT(42)))
            .is_err());
    }

    #[test]
    fn test_property_tombstoning() {
        // Make sure tombstoning works correctly (tracking implicitly deleted descendants).
        // Can delete, but not double-delete, a tombstoned property.
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data.apply(&create_node!(parent: 1, id: 2, name: "second")).is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data.apply(&delete_node!(id: 2)).is_ok());
        assert!(data.apply(&delete_node!(id: 2)).is_err());
        // Can tombstone, then delete, then create.
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data.apply(&create_node!(parent: 1, id: 2, name: "second")).is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data.apply(&delete_node!(id: 2)).is_ok());
        assert!(data.apply(&create_node!(parent: 0, id: 2, name: "new_root_second")).is_ok());
        // Cannot tombstone, then create.
        let mut data = Data::new();
        assert!(data.apply(&create_node!(parent: 0, id: 1, name: "first")).is_ok());
        assert!(data.apply(&create_node!(parent: 1, id: 2, name: "second")).is_ok());
        assert!(data.apply(&delete_node!(id: 1)).is_ok());
        assert!(data.apply(&create_node!(parent: 0, id: 2, name: "new_root_second")).is_err());
    }

    #[test]
    fn diff_modes_work() -> Result<(), Error> {
        let mut local = Data::new();
        let mut remote = Data::new();
        local.apply(&create_node!(parent: 0, id: 1, name: "node"))?;
        local.apply(&create_string_property!(parent: 1, id: 2, name: "prop1", value: "foo"))?;
        remote.apply(&create_node!(parent: 0, id: 1, name: "node"))?;
        remote.apply(&create_string_property!(parent: 1, id: 2, name: "prop1", value: "bar"))?;
        let diff_string = "-- DIFF --\n\nSame:3 lines\nLocal: > >  prop1: \
             String(\"foo\")\nOther: > >  prop1: String(\"bar\")\nSame:3 lines\n\n";
        let full_string = "-- LOCAL --\n root ->\n\n>  node ->\n> >  prop1: String(\"foo\")\
             \n\n\n\n-- OTHER --\n root ->\n\n>  node ->\n> >  prop1: \
             String(\"bar\")\n\n\n\n";
        match local.compare(&mut remote, DiffType::Diff) {
            Err(error) => {
                let error_string = format!("{:?}", error);
                assert!(error_string.contains(diff_string));
                assert!(!error_string.contains(full_string));
            }
            _ => return Err(format_err!("Didn't get failure")),
        }
        match local.compare(&mut remote, DiffType::Full) {
            Err(error) => {
                let error_string = format!("{:?}", error);
                assert!(
                    error_string.contains(full_string),
                    format!("\n{}\n{}\n", error_string, full_string)
                );
                assert!(!error_string.contains(&diff_string));
            }
            _ => return Err(format_err!("Didn't get failure")),
        }
        match local.compare(&mut remote, DiffType::Both) {
            Err(error) => {
                let error_string = format!("{:?}", error);
                assert!(error_string.contains(full_string), full_string);
                eprintln!("e2: {}", error_string);
                eprintln!("d2: {}", diff_string);
                assert!(error_string.contains(&diff_string));
            }
            _ => return Err(format_err!("Didn't get failure")),
        }
        Ok(())
    }
}
