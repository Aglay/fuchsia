// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[derive(Debug, Clone, PartialEq)]
pub enum Definition {
    Command(Command),
    Response { name: String, is_extension: bool, arguments: Arguments },
    Enum { name: String, variants: Vec<Variant> },
}

impl Definition {
    pub fn is_command(&self) -> bool {
        if let Definition::Command(_) = self {
            true
        } else {
            false
        }
    }

    pub fn is_response(&self) -> bool {
        if let Definition::Response { .. } = self {
            true
        } else {
            false
        }
    }

    pub fn is_enum(&self) -> bool {
        if let Definition::Enum { .. } = self {
            true
        } else {
            false
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Command {
    Execute { name: String, is_extension: bool, arguments: Option<ExecuteArguments> },
    Read { name: String, is_extension: bool },
    Test { name: String, is_extension: bool },
}

#[derive(Debug, Clone, PartialEq)]
pub struct ExecuteArguments {
    pub nonstandard_delimiter: Option<String>,
    pub arguments: Arguments,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Arguments {
    ParenthesisDelimitedArgumentLists(Vec<Vec<Argument>>),
    ArgumentList(Vec<Argument>),
}

impl Arguments {
    pub fn is_empty(&self) -> bool {
        match self {
            Self::ArgumentList(vec) => vec.is_empty(),
            Self::ParenthesisDelimitedArgumentLists(vec) => {
                vec.is_empty() || vec.into_iter().all(|el| el.is_empty())
            }
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Argument {
    pub name: String,
    pub typ: Type,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Type {
    List(PrimitiveType),
    Map { key: PrimitiveType, value: PrimitiveType },
    PrimitiveType(PrimitiveType),
}

#[derive(Debug, Clone, PartialEq)]
pub enum PrimitiveType {
    String,
    Integer,
    // Special case 1 and 0 representing true and false
    BoolAsInt,
    NamedType(String),
}

#[derive(Debug, Clone, PartialEq)]
pub struct Variant {
    pub name: String,
    pub value: i64,
}
