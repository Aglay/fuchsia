// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    cm_fidl_validator,
    cm_json::{self, cm, Error},
    fidl_fuchsia_data as fdata, fidl_fuchsia_sys2 as fsys,
    serde_json::{Map, Value},
};

/// Converts the contents of a CM file and produces the equivalent FIDL.
/// The mapping between CM-JSON and CM-FIDL is 1-1. The only difference is the language semantics
/// used to express particular data structures.
/// This function also applies cm_fidl_validator to the generated FIDL.
pub fn translate(buffer: &str) -> Result<fsys::ComponentDecl, Error> {
    let json = cm_json::from_json_str(&buffer)?;
    cm_json::validate_json(&json, cm_json::CM_SCHEMA)?;
    let document: cm::Document = serde_json::from_str(&buffer)
        .map_err(|e| Error::parse(format!("Couldn't read input as struct: {}", e)))?;
    let decl = document.cm_into()?;
    cm_fidl_validator::validate(&decl).map_err(|e| Error::validate_fidl(e))?;
    Ok(decl)
}

/// Converts a cm object into its corresponding fidl representation.
trait CmInto<T> {
    fn cm_into(self) -> Result<T, Error>;
}

/// Generates a `CmInto` implementation for `Vec<type>` that calls `cm_into()` on each element.
macro_rules! cm_into_vec {
    ($into_type:ty, $from_type:ty) => {
        impl CmInto<Vec<$into_type>> for Vec<$from_type> {
            fn cm_into(self) -> Result<Vec<$into_type>, Error> {
                let mut out = vec![];
                for e in self.into_iter() {
                    out.push(e.cm_into()?);
                }
                Ok(out)
            }
        }
    };
}

/// Generates a `CmInto` implementation for `Opt<Vec<type>>` that calls `cm_into()` on each element.
macro_rules! cm_into_opt_vec {
    ($into_type:ty, $from_type:ty) => {
        impl CmInto<Option<Vec<$into_type>>> for Option<Vec<$from_type>> {
            fn cm_into(self) -> Result<Option<Vec<$into_type>>, Error> {
                match self {
                    Some(from) => {
                        let mut out = vec![];
                        for e in from.into_iter() {
                            out.push(e.cm_into()?);
                        }
                        Ok(Some(out))
                    }
                    None => Ok(None),
                }
            }
        }
    };
}

cm_into_opt_vec!(fsys::UseDecl, cm::Use);
cm_into_opt_vec!(fsys::ExposeDecl, cm::Expose);
cm_into_opt_vec!(fsys::OfferDecl, cm::Offer);
cm_into_opt_vec!(fsys::ChildDecl, cm::Child);
cm_into_vec!(fsys::OfferTarget, cm::Target);

impl CmInto<fsys::ComponentDecl> for cm::Document {
    fn cm_into(self) -> Result<fsys::ComponentDecl, Error> {
        Ok(fsys::ComponentDecl {
            program: self.program.cm_into()?,
            uses: self.uses.cm_into()?,
            exposes: self.exposes.cm_into()?,
            offers: self.offers.cm_into()?,
            children: self.children.cm_into()?,
            facets: self.facets.cm_into()?,
        })
    }
}

impl CmInto<fsys::UseDecl> for cm::Use {
    fn cm_into(self) -> Result<fsys::UseDecl, Error> {
        Ok(fsys::UseDecl {
            type_: Some(capability_from_str(&self.r#type)?),
            source_path: Some(self.source_path),
            target_path: Some(self.target_path),
        })
    }
}

impl CmInto<fsys::ExposeDecl> for cm::Expose {
    fn cm_into(self) -> Result<fsys::ExposeDecl, Error> {
        Ok(fsys::ExposeDecl {
            type_: Some(capability_from_str(&self.r#type)?),
            source_path: Some(self.source_path),
            source: Some(self.source.cm_into()?),
            target_path: Some(self.target_path),
        })
    }
}

impl CmInto<fsys::OfferDecl> for cm::Offer {
    fn cm_into(self) -> Result<fsys::OfferDecl, Error> {
        Ok(fsys::OfferDecl {
            type_: Some(capability_from_str(&self.r#type)?),
            source_path: Some(self.source_path),
            source: Some(self.source.cm_into()?),
            targets: Some(self.targets.cm_into()?),
        })
    }
}

impl CmInto<fsys::ChildDecl> for cm::Child {
    fn cm_into(self) -> Result<fsys::ChildDecl, Error> {
        Ok(fsys::ChildDecl {
            name: Some(self.name),
            uri: Some(self.uri),
            startup: Some(startup_from_str(&self.startup)?),
        })
    }
}

impl CmInto<fsys::RelativeId> for cm::Source {
    fn cm_into(self) -> Result<fsys::RelativeId, Error> {
        Ok(fsys::RelativeId {
            relation: Some(relation_from_str(&self.relation)?),
            child_name: self.child_name,
        })
    }
}

impl CmInto<fsys::OfferTarget> for cm::Target {
    fn cm_into(self) -> Result<fsys::OfferTarget, Error> {
        Ok(fsys::OfferTarget {
            target_path: Some(self.target_path),
            child_name: Some(self.child_name),
        })
    }
}

impl CmInto<Option<fdata::Dictionary>> for Option<Map<String, Value>> {
    fn cm_into(self) -> Result<Option<fdata::Dictionary>, Error> {
        match self {
            Some(from) => {
                let dict = dictionary_from_map(from)?;
                Ok(Some(dict))
            }
            None => Ok(None),
        }
    }
}

fn dictionary_from_map(in_obj: Map<String, Value>) -> Result<fdata::Dictionary, Error> {
    let mut out = fdata::Dictionary { entries: vec![] };
    for (k, v) in in_obj {
        if let Some(value) = convert_value(v)? {
            out.entries.push(fdata::Entry { key: k, value: Some(value) });
        }
    }
    Ok(out)
}

fn convert_value(v: Value) -> Result<Option<Box<fdata::Value>>, Error> {
    Ok(match v {
        Value::Null => None,
        Value::Bool(b) => Some(Box::new(fdata::Value::Bit(b))),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                Some(Box::new(fdata::Value::Inum(i)))
            } else if let Some(f) = n.as_f64() {
                Some(Box::new(fdata::Value::Fnum(f)))
            } else {
                return Err(Error::Parse(format!("Number is out of range: {}", n)));
            }
        }
        Value::String(s) => Some(Box::new(fdata::Value::Str(s.clone()))),
        Value::Array(a) => {
            let mut values = vec![];
            for v in a {
                if let Some(value) = convert_value(v)? {
                    values.push(Some(value));
                }
            }
            let vector = fdata::Vector { values };
            Some(Box::new(fdata::Value::Vec(vector)))
        }
        Value::Object(o) => {
            let dict = dictionary_from_map(o)?;
            Some(Box::new(fdata::Value::Dict(dict)))
        }
    })
}

fn capability_from_str(value: &str) -> Result<fsys::CapabilityType, Error> {
    match value {
        cm::SERVICE => Ok(fsys::CapabilityType::Service),
        cm::DIRECTORY => Ok(fsys::CapabilityType::Directory),
        _ => Err(Error::parse(format!("Unknown capability type: {}", value))),
    }
}

fn relation_from_str(value: &str) -> Result<fsys::Relation, Error> {
    match value {
        cm::REALM => Ok(fsys::Relation::Realm),
        cm::SELF => Ok(fsys::Relation::Myself),
        cm::CHILD => Ok(fsys::Relation::Child),
        _ => Err(Error::parse(format!("Unknown relation: {}", value))),
    }
}

fn startup_from_str(value: &str) -> Result<fsys::StartupMode, Error> {
    match value {
        cm::LAZY => Ok(fsys::StartupMode::Lazy),
        cm::EAGER => Ok(fsys::StartupMode::Eager),
        _ => Err(Error::parse(format!("Unknown startup mode: {}", value))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cm_json::CM_SCHEMA;
    use serde_json::json;

    fn translate_test(input: serde_json::value::Value, expected_output: fsys::ComponentDecl) {
        let component_decl = translate(&format!("{}", input)).expect("translation failed");
        assert_eq!(component_decl, expected_output);
    }

    fn new_component_decl() -> fsys::ComponentDecl {
        fsys::ComponentDecl {
            program: None,
            uses: None,
            exposes: None,
            offers: None,
            facets: None,
            children: None,
        }
    }

    macro_rules! test_translate {
        (
            $(
                $test_name:ident => {
                    input = $input:expr,
                    output = $output:expr,
                },
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    translate_test($input, $output);
                }
            )+
        }
    }

    #[test]
    fn test_translate_invalid_cm_fails() {
        let input = json!({
            "exposes": [
                {
                    "type": "nothing",
                    "source_path": "/svc/fuchsia.logger.Log",
                    "source": {
                        "relation": "self"
                    },
                    "target_path": "/svc/fuchsia.logger.Log"
                }
            ]
        });

        let expected_res: Result<fsys::ComponentDecl, Error> = Err(Error::validate_schema(
            CM_SCHEMA,
            "Pattern condition is not met at /exposes/0/type",
        ));
        let res = translate(&format!("{}", input));
        assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
    }

    test_translate! {
        test_translate_empty => {
            input = json!({}),
            output = new_component_decl(),
        },
        test_translate_program => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                }
            }),
            output = {
                let program = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "binary".to_string(),
                        value: Some(Box::new(fdata::Value::Str("bin/app".to_string()))),
                    }
                ]};
                let mut decl = new_component_decl();
                decl.program = Some(program);
                decl
            },
        },
        test_translate_dictionary_primitive => {
            input = json!({
                "program": {
                    "string": "bar",
                    "int": -42,
                    "float": 3.14,
                    "bool": true,
                    "ignore": null
                }
            }),
            output = {
                let program = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "bool".to_string(),
                        value: Some(Box::new(fdata::Value::Bit(true))),
                    },
                    fdata::Entry{
                        key: "float".to_string(),
                        value: Some(Box::new(fdata::Value::Fnum(3.14))),
                    },
                    fdata::Entry{
                        key: "int".to_string(),
                        value: Some(Box::new(fdata::Value::Inum(-42))),
                    },
                    fdata::Entry{
                        key: "string".to_string(),
                        value: Some(Box::new(fdata::Value::Str("bar".to_string()))),
                    },
                ]};
                let mut decl = new_component_decl();
                decl.program = Some(program);
                decl
            },
        },
        test_translate_dictionary_nested => {
            input = json!({
                "program": {
                    "obj": {
                        "array": [
                            {
                                "string": "bar"
                            },
                            -42
                        ],
                    },
                    "bool": true
                }
            }),
            output = {
                let dict_inner = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "string".to_string(),
                        value: Some(Box::new(fdata::Value::Str("bar".to_string()))),
                    },
                ]};
                let vector = fdata::Vector{values: vec![
                    Some(Box::new(fdata::Value::Dict(dict_inner))),
                    Some(Box::new(fdata::Value::Inum(-42)))
                ]};
                let dict_outer = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "array".to_string(),
                        value: Some(Box::new(fdata::Value::Vec(vector))),
                    },
                ]};
                let program = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "bool".to_string(),
                        value: Some(Box::new(fdata::Value::Bit(true))),
                    },
                    fdata::Entry{
                        key: "obj".to_string(),
                        value: Some(Box::new(fdata::Value::Dict(dict_outer))),
                    },
                ]};
                let mut decl = new_component_decl();
                decl.program = Some(program);
                decl
            },
        },
        test_translate_uses => {
            input = json!({
                "uses": [
                    {
                        "type": "service",
                        "source_path": "/fonts/CoolFonts",
                        "target_path": "/svc/fuchsia.fonts.Provider"
                    },
                    {
                        "type": "directory",
                        "source_path": "/data/assets",
                        "target_path": "/data/assets"
                    }
                ]
            }),
            output = {
                let uses = vec![
                    fsys::UseDecl{
                        type_: Some(fsys::CapabilityType::Service),
                        source_path: Some("/fonts/CoolFonts".to_string()),
                        target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
                    },
                    fsys::UseDecl{
                        type_: Some(fsys::CapabilityType::Directory),
                        source_path: Some("/data/assets".to_string()),
                        target_path: Some("/data/assets".to_string()),
                    },
                ];
                let mut decl = new_component_decl();
                decl.uses = Some(uses);
                decl
            },
        },
        test_translate_exposes => {
            input = json!({
                "exposes": [
                    {
                        "type": "service",
                        "source_path": "/loggers/fuchsia.logger.Log",
                        "source": {
                            "relation": "child",
                            "child_name": "logger"
                        },
                        "target_path": "/svc/fuchsia.logger.Log"
                    },
                    {
                        "type": "directory",
                        "source_path": "/volumes/blobfs",
                        "source": {
                            "relation": "self"
                        },
                        "target_path": "/volumes/blobfs"
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    }
                ]
            }),
            output = {
                let exposes = vec![
                    fsys::ExposeDecl{
                        type_: Some(fsys::CapabilityType::Service),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Child),
                            child_name: Some("logger".to_string()),
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                    fsys::ExposeDecl{
                        type_: Some(fsys::CapabilityType::Directory),
                        source_path: Some("/volumes/blobfs".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Myself),
                            child_name: None,
                        }),
                        target_path: Some("/volumes/blobfs".to_string()),
                    },
                ];
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                    },
                ];
                let mut decl = new_component_decl();
                decl.exposes = Some(exposes);
                decl.children = Some(children);
                decl
            },
        },
        test_translate_offers => {
            input = json!({
                "offers": [
                    {
                        "type": "directory",
                        "source_path": "/data/assets",
                        "source": {
                            "relation": "realm"
                        },
                        "targets": [
                            {
                                "target_path": "/data/realm_assets",
                                "child_name": "logger"
                            },
                            {
                                "target_path": "/data/assets",
                                "child_name": "netstack"
                            }
                        ]
                    },
                    {
                        "type": "directory",
                        "source_path": "/data/config",
                        "source": {
                            "relation": "self"
                        },
                        "targets": [
                            {
                                "target_path": "/data/config",
                                "child_name": "netstack"
                            }
                        ]
                    },
                    {
                        "type": "service",
                        "source_path": "/svc/fuchsia.logger.Log",
                        "source": {
                            "relation": "child",
                            "child_name": "logger"
                        },
                        "targets": [
                            {
                                "target_path": "/svc/fuchsia.logger.SysLog",
                                "child_name": "netstack"
                            }
                        ]
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy",
                    },
                    {
                        "name": "netstack",
                        "uri": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
                        "startup": "eager",
                    }
                ],
            }),
            output = {
                let offers = vec![
                    fsys::OfferDecl{
                        type_: Some(fsys::CapabilityType::Directory),
                        source_path: Some("/data/assets".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Realm),
                            child_name: None
                        }),
                        targets: Some(vec![
                            fsys::OfferTarget{
                                target_path: Some("/data/realm_assets".to_string()),
                                child_name: Some("logger".to_string()),
                            },
                            fsys::OfferTarget{
                                target_path: Some("/data/assets".to_string()),
                                child_name: Some("netstack".to_string()),
                            },
                        ]),
                    },
                    fsys::OfferDecl{
                        type_: Some(fsys::CapabilityType::Directory),
                        source_path: Some("/data/config".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Myself),
                            child_name: None
                        }),
                        targets: Some(vec![
                            fsys::OfferTarget{
                                target_path: Some("/data/config".to_string()),
                                child_name: Some("netstack".to_string()),
                            },
                        ]),
                    },
                    fsys::OfferDecl{
                        type_: Some(fsys::CapabilityType::Service),
                        source_path: Some("/svc/fuchsia.logger.Log".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Child),
                            child_name: Some("logger".to_string()),
                        }),
                        targets: Some(vec![
                            fsys::OfferTarget{
                                target_path: Some("/svc/fuchsia.logger.SysLog".to_string()),
                                child_name: Some("netstack".to_string()),
                            },
                        ]),
                    },
                ];
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                    },
                    fsys::ChildDecl{
                        name: Some("netstack".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(fsys::StartupMode::Eager),
                    },
                ];
                let mut decl = new_component_decl();
                decl.offers = Some(offers);
                decl.children = Some(children);
                decl
            },
        },
        test_translate_children => {
            input = json!({
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    },
                    {
                        "name": "echo_server",
                        "uri": "fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm",
                        "startup": "eager"
                    }
                ]
            }),
            output = {
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                    },
                    fsys::ChildDecl{
                        name: Some("echo_server".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm".to_string()),
                        startup: Some(fsys::StartupMode::Eager),
                    },
                ];
                let mut decl = new_component_decl();
                decl.children = Some(children);
                decl
            },
        },
        test_translate_facets => {
            input = json!({
                "facets": {
                    "authors": [
                        "me",
                        "you"
                    ],
                    "title": "foo",
                    "year": 2018
                }
            }),
            output = {
                let vector = fdata::Vector{values: vec![
                    Some(Box::new(fdata::Value::Str("me".to_string()))),
                    Some(Box::new(fdata::Value::Str("you".to_string()))),
                ]};
                let facets = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "authors".to_string(),
                        value: Some(Box::new(fdata::Value::Vec(vector))),
                    },
                    fdata::Entry{
                        key: "title".to_string(),
                        value: Some(Box::new(fdata::Value::Str("foo".to_string()))),
                    },
                    fdata::Entry{
                        key: "year".to_string(),
                        value: Some(Box::new(fdata::Value::Inum(2018))),
                    },
                ]};
                let mut decl = new_component_decl();
                decl.facets = Some(facets);
                decl
            },
        },
        test_translate_all_sections => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                },
                "uses": [
                    {
                        "type": "service",
                        "source_path": "/fonts/CoolFonts",
                        "target_path": "/svc/fuchsia.fonts.Provider"
                    }
                ],
                "exposes": [
                    {
                        "type": "directory",
                        "source_path": "/volumes/blobfs",
                        "source": {
                            "relation": "self"
                        },
                        "target_path": "/volumes/blobfs"
                    }
                ],
                "offers": [
                    {
                        "type": "service",
                        "source_path": "/svc/fuchsia.logger.Log",
                        "source": {
                            "relation": "child",
                            "child_name": "logger"
                        },
                        "targets": [
                            {
                                "target_path": "/svc/fuchsia.logger.Log",
                                "child_name": "netstack"
                            }
                        ]
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                        "startup": "lazy"
                    },
                    {
                        "name": "netstack",
                        "uri": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
                        "startup": "eager"
                    }
                ],
                "facets": {
                    "author": "Fuchsia",
                    "year": 2018
                }
            }),
            output = {
                let program = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "binary".to_string(),
                        value: Some(Box::new(fdata::Value::Str("bin/app".to_string()))),
                    },
                ]};
                let uses = vec![
                    fsys::UseDecl{
                        type_: Some(fsys::CapabilityType::Service),
                        source_path: Some("/fonts/CoolFonts".to_string()),
                        target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
                    },
                ];
                let exposes = vec![
                    fsys::ExposeDecl{
                        type_: Some(fsys::CapabilityType::Directory),
                        source_path: Some("/volumes/blobfs".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Myself),
                            child_name: None,
                        }),
                        target_path: Some("/volumes/blobfs".to_string()),
                    },
                ];
                let offers = vec![
                    fsys::OfferDecl{
                        type_: Some(fsys::CapabilityType::Service),
                        source_path: Some("/svc/fuchsia.logger.Log".to_string()),
                        source: Some(fsys::RelativeId{
                            relation: Some(fsys::Relation::Child),
                            child_name: Some("logger".to_string()),
                        }),
                        targets: Some(vec![
                            fsys::OfferTarget{
                                target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                                child_name: Some("netstack".to_string()),
                            },
                        ]),
                    },
                ];
                let children = vec![
                    fsys::ChildDecl{
                        name: Some("logger".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
                        startup: Some(fsys::StartupMode::Lazy),
                    },
                    fsys::ChildDecl{
                        name: Some("netstack".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(fsys::StartupMode::Eager),
                    },
                ];
                let facets = fdata::Dictionary{entries: vec![
                    fdata::Entry{
                        key: "author".to_string(),
                        value: Some(Box::new(fdata::Value::Str("Fuchsia".to_string()))),
                    },
                    fdata::Entry{
                        key: "year".to_string(),
                        value: Some(Box::new(fdata::Value::Inum(2018))),
                    },
                ]};
                fsys::ComponentDecl{
                    program: Some(program),
                    uses: Some(uses),
                    exposes: Some(exposes),
                    offers: Some(offers),
                    children: Some(children),
                    facets: Some(facets)
                }
            },
        },
    }
}
