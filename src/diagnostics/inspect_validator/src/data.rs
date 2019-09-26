// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::validate::{self, Number, ROOT_ID},
    failure::{bail, format_err, Error},
    fuchsia_inspect::{
        self,
        format::{
            block::{ArrayFormat, Block, PropertyFormat},
            block_type::BlockType,
        },
        reader as ireader,
    },
    fuchsia_zircon::Vmo,
    num_derive::{FromPrimitive, ToPrimitive},
    std::{
        self,
        cmp::min,
        collections::{HashMap, HashSet},
        convert::{TryFrom, TryInto},
    },
};

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
// When a Data is created from a VMO, the keys are the indexes of the relevant
// blocks. Thus, they will never collide.
//
// Reading from a VMO is a complicated process.
// 1) Try to take a fuchsia_inspect::reader::snapshot::Snapshot of the VMO.
// 2) Iterate through it, pedantically examining all its blocks and loading
//   the relevant blocks into a ScannedObjects structure (which contains
//   ScannedNode, ScannedName, ScannedProperty, and ScannedExtent).
// 2.5) ScannedNodes may be added before they're scanned, since they need to
//   track their child nodes and properties. In this case, their "validated"
//   field will be false until they're actually scanned.
// 3) Starting from the "0" node, create Node and Property objects for all the
//   dependent children and properties (verifying that all dependent objects
//   exist (and are valid in the case of Nodes)). This is also when Extents are
//   combined into byte vectors, and in the case of String, verified to be valid UTF-8.
// 4) Add the Node and Property objects (each with its ID) into the "nodes" and
//   "properties" HashMaps of a new Data object. Note that these HashMaps do not
//   hold the hierarchical information; instead, each Node contains a HashSet of
//   the keys of its children and properties.
//
// In both Data-from-Actions and Data-from-VMO, the "root" node is virtual; nodes
// and properties with a "parent" ID of 0 are directly under the "root" of the tree.
// A placeholder Node is placed at index 0 upon creation to hold the real Nodes and
// properties added during scanning VMO or processing Actions.

#[derive(Debug)]
struct Node {
    name: String,
    parent: u32,
    children: HashSet<u32>,
    properties: HashSet<u32>,
}

#[derive(Debug)]
struct Property {
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
    IntArray(Vec<i64>, ArrayFormat),
    UintArray(Vec<u64>, ArrayFormat),
    DoubleArray(Vec<f64>, ArrayFormat),
}

impl Property {
    fn to_string(&self, prefix: &str) -> String {
        format!("{} {}: {:?}", prefix, self.name, &self.payload)
    }
}

impl Node {
    fn to_string(&self, prefix: &str, tree: &Data) -> String {
        let sub_prefix = format!("{}> ", prefix);
        let mut nodes = vec![];
        for node_id in self.children.iter() {
            nodes.push(
                tree.nodes
                    .get(node_id)
                    .map_or("Missing child".into(), |n| n.to_string(&sub_prefix, tree)),
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
        format!("{} {} ->\n{}\n{}\n", prefix, self.name, properties.join("\n"), nodes.join("\n"))
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
                    unknown => bail!("Unknown number type {:?}", unknown),
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
                    unexpected => bail!("Bad types in CreateLinearHistogram: {:?}", unexpected),
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
                        bail!("Bad types in CreateExponentialHistogram: {:?}", unexpected)
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
                        unexpected => bail!("Type mismatch {:?} trying to insert", unexpected),
                    }
                } else {
                    bail!("Tried to insert number on nonexistent property {}", id);
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
                            bail!("Type mismatch {:?} trying to insert multiple", unexpected)
                        }
                    }
                } else {
                    bail!("Tried to insert_multiple number on nonexistent property {}", id);
                }
            }
            _ => bail!("Unknown action {:?}", action),
        }
    }

    fn create_node(&mut self, parent: u32, id: u32, name: &str) -> Result<(), Error> {
        let node = Node {
            name: name.to_owned(),
            parent,
            children: HashSet::new(),
            properties: HashSet::new(),
        };
        if let Some(_) = self.nodes.insert(id, node) {
            bail!("Create called when node already existed at {}", id);
        }
        if let Some(parent_node) = self.nodes.get_mut(&parent) {
            parent_node.children.insert(id);
        } else {
            bail!("Parent {} of created node {} doesn't exist", parent, id);
        }
        Ok(())
    }

    fn delete_node(&mut self, id: u32) -> Result<(), Error> {
        if id == 0 {
            bail!("Do not try to delete node 0");
        }
        if let Some(node) = self.nodes.remove(&id) {
            // It's legal for a parent to be deleted first, so the parent may not exist when
            // the child is deleted; this isn't an error.
            if let Some(parent) = self.nodes.get_mut(&node.parent) {
                if !parent.children.remove(&id) {
                    // Most of these can only happen in case of internal logic errors.
                    // I can't think of a way to test them; I think the errors are
                    // actually impossible. Should I leave them untested? Remove them
                    // from the code? Add a special test_cfg make_illegal_node()
                    // function just to test them?
                    bail!("Parent {} didn't know about this child {}", node.parent, id);
                }
            }
        // Don't delete descendents.
        // They won't be reached during to_string() and they should be deleted
        // explicitly later. RAII puppets certainly won't delete them implicity
        // and Dart should be happy with any deletion order.
        } else {
            bail!("Delete called on node that doesn't exist: {}", id);
        }
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
            bail!("Parent {} of property {} not found", parent, id);
        }
        let property = Property { parent, id, name: name.into(), payload };
        if let Some(_) = self.properties.insert(id, property) {
            bail!("Property insert called on existing id {}", id);
        }
        Ok(())
    }

    fn delete_property(&mut self, id: u32) -> Result<(), Error> {
        if let Some(property) = self.properties.remove(&id) {
            if let Some(node) = self.nodes.get_mut(&property.parent) {
                if !node.properties.remove(&id) {
                    bail!("Property {}'s parent {} didn't have it as child", id, property.parent);
                }
            } else {
                bail!("Property {}'s parent {} doesn't exist on delete", id, property.parent);
            }
        } else {
            bail!("Tried to delete nonexistent property {}", id);
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
                unexpected => bail!("Bad types {:?} trying to set number", unexpected),
            }
        } else {
            bail!("Tried to {} number on nonexistent property {}", op.name, id);
        }
        Ok(())
    }

    fn check_index<T>(numbers: &Vec<T>, index: usize) -> Result<(), Error> {
        if index >= numbers.len() as usize {
            bail!("Index {} too big for vector length {}", index, numbers.len());
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
                unexpected => bail!("Bad types {:?} trying to set number", unexpected),
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
                unexpected => bail!("Type mismatch {:?} trying to set number", unexpected),
            }
        } else {
            bail!("Tried to {} number on nonexistent property {}", op.name, id);
        }
        Ok(())
    }

    fn set_string(&mut self, id: u32, value: &String) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match &property {
                Property { payload: Payload::String(_), .. } => {
                    property.payload = Payload::String(value.to_owned())
                }
                unexpected => bail!("Bad type {:?} trying to set string", unexpected),
            }
        } else {
            bail!("Tried to set string on nonexistent property {}", id);
        }
        Ok(())
    }

    fn set_bytes(&mut self, id: u32, value: &Vec<u8>) -> Result<(), Error> {
        if let Some(property) = self.properties.get_mut(&id) {
            match &property {
                Property { payload: Payload::Bytes(_), .. } => {
                    property.payload = Payload::Bytes(value.to_owned())
                }
                unexpected => bail!("Bad type {:?} trying to set bytes", unexpected),
            }
        } else {
            bail!("Tried to set bytes on nonexistent property {}", id);
        }
        Ok(())
    }

    // ***** Here are the functions to compare two Data (by converting to a
    // ***** fully informative string).

    /// Compares two in-memory Inspect trees, returning Ok(()) if they have the
    /// same data and an Err<> if they are different. The string in the Err<>
    /// may be very large.
    pub fn compare(&self, other: &Data) -> Result<(), Error> {
        if self.to_string() == other.to_string() {
            Ok(())
        } else {
            // TODO(cphoenix): Use the difference crate to diff the strings.
            bail!(
                "Trees differ:\n-- LOCAL --\n{:?}\n-- OTHER --\n{:?}",
                self.to_string(),
                other.to_string()
            );
        }
    }

    /// Generates a string fully representing the Inspect data.
    pub fn to_string(&self) -> String {
        if let Some(node) = self.nodes.get(&ROOT_ID) {
            node.to_string(&"".to_owned(), self)
        } else {
            "No root node; internal error\n".to_owned()
        }
    }

    /// This creates a new Data. Note that the standard "root" node of the VMO API
    /// corresponds to the index-0 node added here.
    pub fn new() -> Data {
        let mut ret = Data { nodes: HashMap::new(), properties: HashMap::new() };
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

    // ***** These functions load from a VMO or byte array.

    /// Loads the Inspect tree data from a byte array in Inspect file format.
    #[cfg(test)]
    pub fn try_from_bytes(bytes: &[u8]) -> Result<Data, Error> {
        let snapshot = ireader::snapshot::Snapshot::try_from(bytes);
        match snapshot {
            Err(e) => Err(e),
            Ok(snapshot) => Self::try_from_snapshot(snapshot),
        }
    }

    /// Loads the Inspect tree data from a VMO in Inspect file format.
    pub fn try_from_vmo(vmo: &Vmo) -> Result<Data, Error> {
        let snapshot = ireader::snapshot::Snapshot::try_from(vmo);
        match snapshot {
            Err(e) => Err(e),
            Ok(snapshot) => Self::try_from_snapshot(snapshot),
        }
    }

    fn try_from_snapshot(snapshot: ireader::snapshot::Snapshot) -> Result<Data, Error> {
        let mut objects = ScannedObjects::new();
        for block in snapshot.scan() {
            match block.block_type_or() {
                Ok(BlockType::Free) => objects.validate_free(block)?,
                Ok(BlockType::Reserved) => (), // Any contents are valid
                Ok(BlockType::Header) => objects.validate_header(block)?,
                Ok(BlockType::NodeValue) => objects.process_node(block)?,
                Ok(BlockType::IntValue)
                | Ok(BlockType::UintValue)
                | Ok(BlockType::DoubleValue)
                | Ok(BlockType::ArrayValue)
                | Ok(BlockType::PropertyValue) => objects.process_property(block)?,
                Ok(BlockType::Extent) => objects.process_extent(block)?,
                Ok(BlockType::Name) => objects.process_name(block)?,
                Ok(BlockType::Tombstone) => objects.validate_tombstone(block)?,
                Err(error) => return Err(error),
            }
        }
        let (mut new_nodes, mut new_properties) = objects.make_valid_node_tree(ROOT_ID)?;
        let mut nodes = HashMap::new();
        for (node, id) in new_nodes.drain(..) {
            nodes.insert(id, node);
        }
        let mut properties = HashMap::new();
        for (property, id) in new_properties.drain(..) {
            properties.insert(id, property);
        }
        Ok(Data { nodes, properties })
    }
}

#[derive(Debug)]
struct ScannedObjects {
    nodes: HashMap<u32, ScannedNode>,
    names: HashMap<u32, ScannedName>,
    properties: HashMap<u32, ScannedProperty>,
    extents: HashMap<u32, ScannedExtent>,
}

impl ScannedObjects {
    // ***** Utility functions

    fn new() -> ScannedObjects {
        let mut objects = ScannedObjects {
            nodes: HashMap::new(),
            names: HashMap::new(),
            properties: HashMap::new(),
            extents: HashMap::new(),
        };
        // The ScannedNode at 0 is the "root" node. It exists to receive pointers to objects
        // whose parent is 0 while scanning the VMO.
        objects.nodes.insert(
            0,
            ScannedNode {
                validated: true,
                parent: 0,
                name: 0,
                children: HashSet::new(),
                properties: HashSet::new(),
            },
        );
        objects
    }

    fn get_node(&self, node_id: u32) -> Result<&ScannedNode, Error> {
        self.nodes.get(&node_id).ok_or(format_err!("No node at index {}", node_id))
    }

    fn get_property(&self, property_id: u32) -> Result<&ScannedProperty, Error> {
        self.properties.get(&property_id).ok_or(format_err!("No property at index {}", property_id))
    }

    fn get_owned_name(&self, name_id: u32) -> Result<String, Error> {
        Ok(self
            .names
            .get(&name_id)
            .ok_or(format_err!("No string at index {}", name_id))?
            .name
            .clone())
    }

    // ***** Functions which read fuchsia_inspect::format::block::Block (actual
    // ***** VMO blocks), validate them, turn them into Scanned* objects, and
    // ***** add the ones we care about to Self.

    // TODO(cphoenix): Add full pedantic/paranoid checking on all
    // validate_ and process_ functions.
    // Note: validate_ and process_ functions are only called from the scan() iterator on the
    // VMO's blocks, so indexes of the blocks themselves will never be duplicated; that's one
    // thing we don't have to verify.
    fn validate_free(&self, _block: Block<&[u8]>) -> Result<(), Error> {
        Ok(())
    }

    fn validate_header(&self, _block: Block<&[u8]>) -> Result<(), Error> {
        Ok(())
    }

    fn validate_tombstone(&self, _block: Block<&[u8]>) -> Result<(), Error> {
        Ok(())
    }

    fn process_extent(&mut self, block: Block<&[u8]>) -> Result<(), Error> {
        self.extents.insert(
            block.index(),
            ScannedExtent { next: block.next_extent()?, data: block.extent_contents()? },
        );
        Ok(())
    }

    fn process_name(&mut self, block: Block<&[u8]>) -> Result<(), Error> {
        self.names.insert(block.index(), ScannedName { name: block.name_contents()? });
        Ok(())
    }

    fn process_node(&mut self, block: Block<&[u8]>) -> Result<(), Error> {
        let parent = block.parent_index()?;
        let id = block.index();
        let name = block.name_index()?;
        let mut node;
        if let Some(placeholder) = self.nodes.remove(&id) {
            // We need to preserve the children and properties.
            node = placeholder;
            node.validated = true;
            node.parent = parent;
            node.name = name;
        } else {
            node = ScannedNode {
                validated: true,
                name,
                parent,
                children: HashSet::new(),
                properties: HashSet::new(),
            }
        }
        self.nodes.insert(id, node);
        self.add_to_parent(parent, id, |node| &mut node.children);
        Ok(())
    }

    fn add_to_parent<F: FnOnce(&mut ScannedNode) -> &mut HashSet<u32>>(
        &mut self,
        parent: u32,
        id: u32,
        get_the_hashset: F, // Gets children or properties
    ) {
        if !self.nodes.contains_key(&parent) {
            self.nodes.insert(
                parent,
                ScannedNode {
                    validated: false,
                    name: 0,
                    parent: 0,
                    children: HashSet::new(),
                    properties: HashSet::new(),
                },
            );
        }
        if let Some(parent_node) = self.nodes.get_mut(&parent) {
            get_the_hashset(parent_node).insert(id);
        }
    }

    fn build_scanned_payload(
        block: &Block<&[u8]>,
        block_type: BlockType,
    ) -> Result<ScannedPayload, Error> {
        Ok(match block_type {
            BlockType::IntValue => ScannedPayload::Int(block.int_value()?),
            BlockType::UintValue => ScannedPayload::Uint(block.uint_value()?),
            BlockType::DoubleValue => ScannedPayload::Double(block.double_value()?),
            BlockType::PropertyValue => {
                let format = block.property_format()?;
                let length = block.property_total_length()?;
                let link = block.property_extent_index()?;
                match format {
                    PropertyFormat::String => ScannedPayload::String { length, link },
                    PropertyFormat::Bytes => ScannedPayload::Bytes { length, link },
                }
            }
            BlockType::ArrayValue => {
                let entry_type = block.array_entry_type()?;
                let array_format = block.array_format()?;
                let slots = block.array_slots()? as usize;
                match entry_type {
                    BlockType::IntValue => {
                        let numbers: Result<Vec<i64>, _> =
                            (0..slots).map(|i| block.array_get_int_slot(i)).collect();
                        ScannedPayload::IntArray(numbers?, array_format)
                    }
                    BlockType::UintValue => {
                        let numbers: Result<Vec<u64>, _> =
                            (0..slots).map(|i| block.array_get_uint_slot(i)).collect();
                        ScannedPayload::UintArray(numbers?, array_format)
                    }
                    BlockType::DoubleValue => {
                        let numbers: Result<Vec<f64>, _> =
                            (0..slots).map(|i| block.array_get_double_slot(i)).collect();
                        ScannedPayload::DoubleArray(numbers?, array_format)
                    }
                    illegal_type => {
                        bail!("No way I should see {:?} for ArrayEntryType", illegal_type)
                    }
                }
            }
            illegal_type => bail!("No way I should see {:?} for BlockType", illegal_type),
        })
    }

    fn process_property(&mut self, block: Block<&[u8]>) -> Result<(), Error> {
        let id = block.index();
        let parent = block.parent_index()?;
        let block_type = block.block_type_or()?;
        let payload = Self::build_scanned_payload(&block, block_type)?;
        let property = ScannedProperty { name: block.name_index()?, parent, payload };
        self.properties.insert(id, property);
        self.add_to_parent(parent, id, |node| &mut node.properties);
        Ok(())
    }

    // ***** Functions which convert Scanned* objects into Node and Property objects.

    fn make_valid_node_tree(
        &self,
        id: u32,
    ) -> Result<(Vec<(Node, u32)>, Vec<(Property, u32)>), Error> {
        let scanned_node = self.get_node(id)?;
        if !scanned_node.validated {
            bail!("No node at {}", id)
        }
        let mut nodes_in_tree = vec![];
        let mut properties_under = vec![];
        for node_id in scanned_node.children.iter() {
            let (mut nodes_of, mut properties_of) = self.make_valid_node_tree(*node_id)?;
            nodes_in_tree.append(&mut nodes_of);
            properties_under.append(&mut properties_of);
        }
        for property_id in scanned_node.properties.iter() {
            properties_under.push((self.make_valid_property(*property_id)?, *property_id));
        }
        let name =
            if id == 0 { ROOT_NAME.to_owned() } else { self.get_owned_name(scanned_node.name)? };
        let this_node = Node {
            name,
            parent: scanned_node.parent,
            children: scanned_node.children.clone(),
            properties: scanned_node.properties.clone(),
        };
        nodes_in_tree.push((this_node, id));
        Ok((nodes_in_tree, properties_under))
    }

    fn make_valid_property(&self, id: u32) -> Result<Property, Error> {
        let scanned_property = self.get_property(id)?;
        let name = self.get_owned_name(scanned_property.name)?;
        let payload = self.make_valid_payload(&scanned_property.payload)?;
        Ok(Property { id, name, parent: scanned_property.parent, payload })
    }

    fn make_valid_payload(&self, payload: &ScannedPayload) -> Result<Payload, Error> {
        Ok(match payload {
            ScannedPayload::Int(data) => Payload::Int(*data),
            ScannedPayload::Uint(data) => Payload::Uint(*data),
            ScannedPayload::Double(data) => Payload::Double(*data),
            ScannedPayload::IntArray(data, format) => {
                Payload::IntArray(data.clone(), format.clone())
            }
            ScannedPayload::UintArray(data, format) => {
                Payload::UintArray(data.clone(), format.clone())
            }
            ScannedPayload::DoubleArray(data, format) => {
                Payload::DoubleArray(data.clone(), format.clone())
            }
            ScannedPayload::Bytes { length, link } => {
                Payload::Bytes(self.make_valid_vector(*length, *link)?)
            }
            ScannedPayload::String { length, link } => Payload::String(
                std::str::from_utf8(&self.make_valid_vector(*length, *link)?)?.to_owned(),
            ),
        })
    }

    fn make_valid_vector(&self, length: usize, link: u32) -> Result<Vec<u8>, Error> {
        let mut dest = vec![];
        let mut length_remaining = length;
        let mut next_link = link;
        while length_remaining > 0 {
            let extent =
                self.extents.get(&next_link).ok_or(format_err!("No extent at {}", next_link))?;
            let copy_len = min(extent.data.len(), length_remaining);
            dest.extend_from_slice(&extent.data[..copy_len]);
            length_remaining -= copy_len;
            next_link = extent.next;
        }
        Ok(dest)
    }
}

#[derive(Debug)]
struct ScannedNode {
    // These may be created two ways: Either from being named as a parent, or
    // from being processed in the VMO. Those named but not yet processed will
    // have validated = false. Of course after a complete VMO scan,
    // everything descended from a root node must be validated.
    // validated refers to the binary contents of this block; it doesn't
    // guarantee that properties, descendents, name, etc. are valid.
    validated: bool,
    name: u32,
    parent: u32,
    children: HashSet<u32>,
    properties: HashSet<u32>,
}

#[derive(Debug)]
struct ScannedProperty {
    name: u32,
    parent: u32,
    payload: ScannedPayload,
}

#[derive(Debug)]
struct ScannedName {
    name: String,
}

#[derive(Debug)]
struct ScannedExtent {
    next: u32,
    data: Vec<u8>,
}

#[derive(Debug)]
enum ScannedPayload {
    String { length: usize, link: u32 },
    Bytes { length: usize, link: u32 },
    Int(i64),
    Uint(u64),
    Double(f64),
    IntArray(Vec<i64>, ArrayFormat),
    UintArray(Vec<u64>, ArrayFormat),
    DoubleArray(Vec<f64>, ArrayFormat),
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
        fuchsia_async as fasync,
        fuchsia_inspect::format::{bitfields::BlockHeader, constants},
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

    fn copy_into(source: &[u8], dest: &mut [u8], offset: usize) {
        dest[offset..offset + source.len()].copy_from_slice(source);
    }

    // Run "fx run-test inspect_validator_tests -- --nocapture" to see all the output
    // and verify you're getting appropriate error messages for each tweaked byte.
    // (The alternative is hard-coding expected error strings, which is possible but ugh.)
    fn try_byte(
        buffer: &mut [u8],
        (index, offset): (usize, usize),
        value: u8,
        predicted: Option<&str>,
    ) {
        let location = index * 16 + offset;
        let previous = buffer[location];
        buffer[location] = value;
        let actual = Data::try_from_bytes(buffer).map(|d| d.to_string());
        if predicted.is_none() {
            if actual.is_err() {
                println!(
                    "With ({},{}) -> {}, got expected error {:?}",
                    index, offset, value, actual
                );
            } else {
                println!(
                    "BAD: With ({},{}) -> {}, expected error but got string {:?}",
                    index,
                    offset,
                    value,
                    actual.as_ref().unwrap()
                );
            }
        } else {
            if actual.is_err() {
                println!(
                    "BAD: With ({},{}) -> {}, got unexpected error {:?}",
                    index, offset, value, actual
                );
            } else if actual.as_ref().ok().map(|s| &s[..]) == predicted {
                println!(
                    "With ({},{}) -> {}, got expected string {:?}",
                    index,
                    offset,
                    value,
                    predicted.unwrap()
                );
            } else {
                println!(
                    "BAD: With ({},{}) -> {}, expected string {:?} but got {:?}",
                    index,
                    offset,
                    value,
                    predicted.unwrap(),
                    actual.as_ref().unwrap()
                );
                println!("Raw data: {:?}", Data::try_from_bytes(buffer))
            }
        }
        assert_eq!(predicted, actual.as_ref().ok().map(|s| &s[..]));
        buffer[location] = previous;
    }

    fn put_header(header: &BlockHeader, buffer: &mut [u8], index: usize) {
        copy_into(&header.value().to_le_bytes(), buffer, index * 16);
    }

    #[test]
    fn test_scanning_logic() {
        let mut buffer = [0u8; 4096];
        // VMO Header block (index 0)
        const HEADER: usize = 0;
        let mut header = BlockHeader(0);
        header.set_order(0);
        header.set_block_type(BlockType::Header.to_u8().unwrap());
        header.set_header_magic(constants::HEADER_MAGIC_NUMBER);
        header.set_header_version(constants::HEADER_VERSION_NUMBER);
        put_header(&header, &mut buffer, HEADER);
        const ROOT: usize = 1;
        let mut header = BlockHeader(0);
        header.set_order(0);
        header.set_block_type(BlockType::NodeValue.to_u8().unwrap());
        header.set_value_name_index(2);
        header.set_value_parent_index(0);
        put_header(&header, &mut buffer, ROOT);
        // Root's Name block
        const ROOT_NAME: usize = 2;
        let mut header = BlockHeader(0);
        header.set_order(0);
        header.set_block_type(BlockType::Name.to_u8().unwrap());
        header.set_name_length(4);
        put_header(&header, &mut buffer, ROOT_NAME);
        copy_into(b"node", &mut buffer, ROOT_NAME * 16 + 8);
        try_byte(&mut buffer, (16, 0), 0, Some(" root ->\n\n>  node ->\n\n\n\n"));
        // Mess up HEADER_MAGIC_NUMBER - it should fail to load.
        try_byte(&mut buffer, (HEADER, 7), 0, None);
        // Mess up node's parent; should disappear.
        try_byte(&mut buffer, (ROOT, 1), 1, Some(" root ->\n\n\n"));
        // Mess up root's name; should fail.
        try_byte(&mut buffer, (ROOT, 5), 1, None);
        // Mess up generation count; should fail (and not hang).
        try_byte(&mut buffer, (HEADER, 8), 1, None);
        // But an even generation count should work.
        try_byte(&mut buffer, (HEADER, 8), 2, Some(" root ->\n\n>  node ->\n\n\n\n"));
        // Let's give it a property.
        const NUMBER: usize = 3;
        let mut header = BlockHeader(0);
        header.set_order(0);
        header.set_block_type(BlockType::IntValue.to_u8().unwrap());
        header.set_value_name_index(4);
        header.set_value_parent_index(1);
        put_header(&header, &mut buffer, NUMBER);
        const NUMBER_NAME: usize = 4;
        let mut header = BlockHeader(0);
        header.set_order(0);
        header.set_block_type(BlockType::Name.to_u8().unwrap());
        header.set_name_length(6);
        put_header(&header, &mut buffer, NUMBER_NAME);
        copy_into(b"number", &mut buffer, NUMBER_NAME * 16 + 8);
        try_byte(
            &mut buffer,
            (HEADER, 8),
            2,
            Some(" root ->\n\n>  node ->\n> >  number: Int(0)\n\n\n"),
        );
        try_byte(
            &mut buffer,
            (NUMBER, 0),
            0x50,
            Some(" root ->\n\n>  node ->\n> >  number: Uint(0)\n\n\n"),
        );
        try_byte(
            &mut buffer,
            (NUMBER, 0),
            0x60,
            Some(" root ->\n\n>  node ->\n> >  number: Double(0.0)\n\n\n"),
        );
        try_byte(
            &mut buffer,
            (NUMBER, 0),
            0x70,
            Some(" root ->\n\n>  node ->\n> >  number: String(\"\")\n\n\n"),
        );
        // Array block will have illegal Array Entry Type of 0.
        try_byte(&mut buffer, (NUMBER, 0), 0xb0, None);
        // 15 is an illegal block type.
        try_byte(&mut buffer, (NUMBER, 0), 0xf0, None);

        // TODO: Test more cases here.
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_to_string_order() -> Result<(), Error> {
        // Make sure property payloads are distinguished by name, value, and type
        // but ignore id and parent, and that prefix is used.
        let int0 = Property { name: "int0".into(), id: 2, parent: 1, payload: Payload::Int(0) }
            .to_string("");
        let int1_struct =
            Property { name: "int1".into(), id: 2, parent: 1, payload: Payload::Int(1) };
        let int1 = int1_struct.to_string("");
        assert_ne!(int0, int1);
        let uint0 = Property { name: "uint0".into(), id: 2, parent: 1, payload: Payload::Uint(0) }
            .to_string("");
        assert_ne!(int0, uint0);
        let int0_different_name = Property {
            name: "int0_different_name".into(),
            id: 2,
            parent: 1,
            payload: Payload::Int(0),
        }
        .to_string("");
        assert_ne!(int0, int0_different_name);
        let uint0_different_ids =
            Property { name: "uint0".into(), id: 3, parent: 4, payload: Payload::Uint(0) }
                .to_string("");
        assert_eq!(uint0, uint0_different_ids);
        let int1_different_prefix = int1_struct.to_string("foo");
        assert_ne!(int1, int1_different_prefix);
        // Test that order doesn't matter. Use a real VMO rather than Data's
        // HashMaps which may not reflect order of addition.
        let mut puppet1 = puppet::tests::local_incomplete_puppet().await?;
        let mut child1_action = create_node!(parent:0, id:1, name:"child1");
        let mut child2_action = create_node!(parent:0, id:2, name:"child2");
        let mut property1_action =
            create_numeric_property!(parent:0, id:1, name:"prop1", value: Number::IntT(1));
        let mut property2_action =
            create_numeric_property!(parent:0, id:2, name:"prop2", value: Number::IntT(2));
        puppet1.apply(&mut child1_action).await?;
        puppet1.apply(&mut child2_action).await?;
        let mut puppet2 = puppet::tests::local_incomplete_puppet().await?;
        puppet2.apply(&mut child2_action).await?;
        puppet2.apply(&mut child1_action).await?;
        assert_eq!(puppet1.read_data()?.to_string(), puppet2.read_data()?.to_string());
        puppet1.apply(&mut property1_action).await?;
        puppet1.apply(&mut property2_action).await?;
        puppet2.apply(&mut property2_action).await?;
        puppet2.apply(&mut property1_action).await?;
        assert_eq!(puppet1.read_data()?.to_string(), puppet2.read_data()?.to_string());
        // Make sure the tree distinguishes based on node position
        puppet1 = puppet::tests::local_incomplete_puppet().await?;
        puppet2 = puppet::tests::local_incomplete_puppet().await?;
        let mut subchild2_action = create_node!(parent:1, id:2, name:"child2");
        puppet1.apply(&mut child1_action).await?;
        puppet2.apply(&mut child1_action).await?;
        puppet1.apply(&mut child2_action).await?;
        puppet2.apply(&mut subchild2_action).await?;
        assert_ne!(puppet1.read_data()?.to_string(), puppet2.read_data()?.to_string());
        // ... and property position
        let mut subproperty2_action =
            create_numeric_property!(parent:1, id:2, name:"prop2", value: Number::IntT(1));
        puppet1.apply(&mut child1_action).await?;
        puppet2.apply(&mut child1_action).await?;
        puppet1.apply(&mut property2_action).await?;
        puppet2.apply(&mut subproperty2_action).await?;
        Ok(())
    }
}
