// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::cml;
use crate::validate;
use cm_json::{self, cm, Error, CM_SCHEMA};
use serde::ser::Serialize;
use serde_json;
use serde_json::ser::{CompactFormatter, PrettyFormatter, Serializer};
use std::fs;
use std::io::{Read, Write};
use std::path::PathBuf;
use std::str::from_utf8;

/// Read in a CML file and produce the equivalent CM.
pub fn compile(file: &PathBuf, pretty: bool, output: Option<PathBuf>) -> Result<(), Error> {
    const BAD_EXTENSION: &str = "Input file does not have the component manifest extension (.cm)";
    if let Some(ref path) = output {
        match path.extension() {
            Some(ext) => match ext.to_str() {
                Some("cm") => Ok(()),
                _ => Err(Error::invalid_args(BAD_EXTENSION)),
            },
            None => Err(Error::invalid_args(BAD_EXTENSION)),
        }?;
    }

    let mut buffer = String::new();
    fs::File::open(&file)?.read_to_string(&mut buffer)?;
    let document = validate::validate_cml(&buffer)?;
    let out = compile_cml(document)?;

    let mut res = Vec::new();
    if pretty {
        let mut ser = Serializer::with_formatter(&mut res, PrettyFormatter::with_indent(b"    "));
        out.serialize(&mut ser)
            .map_err(|e| Error::parse(format!("Couldn't serialize JSON: {}", e)))?;
    } else {
        let mut ser = Serializer::with_formatter(&mut res, CompactFormatter {});
        out.serialize(&mut ser)
            .map_err(|e| Error::parse(format!("Couldn't serialize JSON: {}", e)))?;
    }
    if let Some(output_path) = output {
        fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(output_path)?
            .write_all(&res)?;
    } else {
        println!("{}", from_utf8(&res)?);
    }
    // Sanity check that output conforms to CM schema.
    let json = serde_json::from_slice(&res)
        .map_err(|e| Error::parse(format!("Couldn't read output as JSON: {}", e)))?;
    cm_json::validate_json(&json, CM_SCHEMA)?;
    Ok(())
}

fn compile_cml(document: cml::Document) -> Result<cm::Document, Error> {
    let mut out = cm::Document::default();
    if let Some(program) = document.program {
        out.program = Some(program.clone());
    }
    if let Some(r#use) = document.r#use {
        out.uses = Some(translate_use(&r#use)?);
    }
    if let Some(expose) = document.expose {
        out.exposes = Some(translate_expose(&expose)?);
    }
    if let Some(offer) = document.offer {
        out.offers = Some(translate_offer(&offer)?);
    }
    if let Some(children) = document.children {
        out.children = Some(translate_children(&children)?);
    }
    if let Some(facets) = document.facets {
        out.facets = Some(facets.clone());
    }
    Ok(out)
}

fn translate_use(use_in: &Vec<cml::Use>) -> Result<Vec<cm::Use>, Error> {
    let mut out_uses = vec![];
    for use_ in use_in {
        let (r#type, source_path) = extract_source_capability(use_)?;
        let target_path = extract_target_path(use_, &source_path);
        out_uses.push(cm::Use { r#type, source_path, target_path });
    }
    Ok(out_uses)
}

fn translate_expose(expose_in: &Vec<cml::Expose>) -> Result<Vec<cm::Expose>, Error> {
    let mut out_exposes = vec![];
    for expose in expose_in.iter() {
        let (r#type, source_path) = extract_source_capability(expose)?;
        let source = extract_source(expose)?;
        let target_path = extract_target_path(expose, &source_path);
        out_exposes.push(cm::Expose { r#type, source_path, source, target_path });
    }
    Ok(out_exposes)
}

fn translate_offer(offer_in: &Vec<cml::Offer>) -> Result<Vec<cm::Offer>, Error> {
    let mut out_offers = vec![];
    for offer in offer_in.iter() {
        let (r#type, source_path) = extract_source_capability(offer)?;
        let source = extract_source(offer)?;
        let targets = extract_targets(offer, &source_path)?;
        out_offers.push(cm::Offer { r#type, source_path, source, targets });
    }
    Ok(out_offers)
}

fn translate_children(children_in: &Vec<cml::Child>) -> Result<Vec<cm::Child>, Error> {
    let mut out_children = vec![];
    for child in children_in.iter() {
        out_children.push(cm::Child { name: child.name.clone(), uri: child.uri.clone() });
    }
    Ok(out_children)
}

// Extract "source" from "from".
fn extract_source<T>(in_obj: &T) -> Result<cm::Source, Error>
where
    T: cml::FromClause,
{
    let from = in_obj.from().to_string();
    if !cml::FROM_RE.is_match(&from) {
        return Err(Error::internal(format!("invalid \"from\": {}", from)));
    }
    let ret = if from.starts_with("#") {
        let (_, child_name) = from.split_at(1);
        cm::Source { relation: "child".to_string(), child_name: Some(child_name.to_string()) }
    } else {
        cm::Source { relation: from, child_name: None }
    };
    Ok(ret)
}

// Extract "targets" from "targets".
fn extract_targets(in_obj: &cml::Offer, source_path: &str) -> Result<Vec<cm::Target>, Error> {
    let mut out_targets = vec![];
    for target in in_obj.targets.iter() {
        let target_path = extract_target_path(target, source_path);
        let caps = match cml::CHILD_RE.captures(&target.to) {
            Some(c) => Ok(c),
            None => Err(Error::internal(format!("invalid \"to\": {}", target.to))),
        }?;
        let child_name = caps[1].to_string();
        out_targets.push(cm::Target { target_path, child_name });
    }
    Ok(out_targets)
}

fn extract_source_capability<T>(in_obj: &T) -> Result<(String, String), Error>
where
    T: cml::CapabilityClause,
{
    let (capability, source_path) = if let Some(p) = in_obj.service() {
        (cml::SERVICE.to_string(), p.clone())
    } else if let Some(p) = in_obj.directory() {
        (cml::DIRECTORY.to_string(), p.clone())
    } else {
        return Err(Error::internal(format!("no source path")));
    };
    Ok((capability, source_path))
}

fn extract_target_path<T>(in_obj: &T, source_path: &str) -> String
where
    T: cml::AsClause,
{
    if let Some(as_) = in_obj.r#as() {
        as_.clone()
    } else {
        source_path.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::fs::File;
    use std::io;
    use std::io::{Read, Write};
    use tempfile::TempDir;

    macro_rules! test_compile {
        (
            $(
                $test_name:ident => {
                    input = $input:expr,
                    output = $result:expr,
                },
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    compile_test($input, $result, true);
                }
            )+
        }
    }

    fn compile_test(input: serde_json::value::Value, expected_output: &str, pretty: bool) {
        let tmp_dir = TempDir::new().unwrap();
        let tmp_in_path = tmp_dir.path().join("test.cml");
        let tmp_out_path = tmp_dir.path().join("test.cm");

        File::create(&tmp_in_path).unwrap().write_all(format!("{}", input).as_bytes()).unwrap();

        compile(&tmp_in_path, pretty, Some(tmp_out_path.clone())).expect("compilation failed");
        let mut buffer = String::new();
        fs::File::open(&tmp_out_path).unwrap().read_to_string(&mut buffer).unwrap();
        assert_eq!(buffer, expected_output);
    }

    // TODO: Consider converting these to a golden test

    // TODO(viktard): re-enable after json5 0.2.4 merged
    #[ignore]
    test_compile! {
        test_compile_empty => {
            input = json!({}),
            output = "{}",
        },
        test_compile_program => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                }
            }),
            output = r#"{
    "program": {
        "binary": "bin/app"
    }
}"#,
        },
        test_compile_use => {
            input = json!({
                "use": [
                    { "service": "/fonts/CoolFonts", "as": "/svc/fuchsia.fonts.Provider" },
                    { "directory": "/data/assets" }
                ]
            }),
            output = r#"{
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
}"#,
        },
        test_compile_expose => {
            input = json!({
                "expose": [
                    {
                      "service": "/loggers/fuchsia.logger.Log",
                      "from": "#logger",
                      "as": "/svc/fuchsia.logger.Log"
                    },
                    { "directory": "/volumes/blobfs", "from": "self" }
                ],
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                ]
            }),
            output = r#"{
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
            "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
        }
    ]
}"#,
        },
        test_compile_offer => {
            input = json!({
                "offer": [
                    {
                        "service": "/svc/fuchsia.logger.Log",
                        "from": "#logger",
                        "targets": [
                            { "to": "#netstack" },
                            { "to": "#echo_server", "as": "/svc/fuchsia.logger.SysLog" }
                        ]
                    },
                    {
                        "directory": "/data/assets",
                        "from": "realm",
                        "targets": [
                            { "to": "#echo_server" },
                        ]
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                    {
                        "name": "echo_server",
                        "uri": "fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm"
                    },
                    {
                        "name": "netstack",
                        "uri": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm"
                    }
                ]
            }),
            output = r#"{
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
                },
                {
                    "target_path": "/svc/fuchsia.logger.SysLog",
                    "child_name": "echo_server"
                }
            ]
        },
        {
            "type": "directory",
            "source_path": "/data/assets",
            "source": {
                "relation": "realm"
            },
            "targets": [
                {
                    "target_path": "/data/assets",
                    "child_name": "echo_server"
                }
            ]
        }
    ],
    "children": [
        {
            "name": "logger",
            "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
        },
        {
            "name": "echo_server",
            "uri": "fuchsia-pkg://fuchsia.com/echo_server/stable#meta/echo_server.cm"
        },
        {
            "name": "netstack",
            "uri": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm"
        }
    ]
}"#,
        },
        test_compile_children => {
            input = json!({
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                    {
                        "name": "gmail",
                        "uri": "https://www.google.com/gmail"
                    }
                ]
            }),
            output = r#"{
    "children": [
        {
            "name": "logger",
            "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
        },
        {
            "name": "gmail",
            "uri": "https://www.google.com/gmail"
        }
    ]
}"#,
        },
        // TODO(CF-167): JSON5 int->float parse bug

        test_compile_facets => {
            input = json!({
                "facets": {
                    "metadata": {
                        "title": "foo",
                        "authors": [ "me", "you" ],
                        "year": 2018
                    }
                }
            }),
            output = r#"{
    "facets": {
        "metadata": {
            "authors": [
                "me",
                "you"
            ],
            "title": "foo",
            "year": 2018.0
        }
    }
}"#,
        },

        test_compile_all_sections => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                },
                "use": [
                    { "service": "/fonts/CoolFonts", "as": "/svc/fuchsia.fonts.Provider" },
                ],
                "expose": [
                    { "directory": "/volumes/blobfs", "from": "self" }
                ],
                "offer": [
                    {
                        "service": "/svc/fuchsia.logger.Log",
                        "from": "#logger",
                        "targets": [
                          { "to": "#netstack" }
                        ]
                    }
                ],
                "children": [
                    {
                        "name": "logger",
                        "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                    {
                        "name": "netstack",
                        "uri": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm"
                    }
                ],
                "facets": {
                    "author": "Fuchsia",
                    "year": 2018
                }
            }),
            // TODO(CF-167): JSON5 int->float parse bug
            output = r#"{
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
            "uri": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
        },
        {
            "name": "netstack",
            "uri": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm"
        }
    ],
    "facets": {
        "author": "Fuchsia",
        "year": 2018.0
    }
}"#,
        },
    }

    #[test]
    fn test_compile_compact() {
        let input = json!({
            "use": [
                { "service": "/fonts/CoolFonts", "as": "/svc/fuchsia.fonts.Provider" },
                { "directory": "/data/assets" }
            ]
        });
        let output = r#"{"uses":[{"type":"service","source_path":"/fonts/CoolFonts","target_path":"/svc/fuchsia.fonts.Provider"},{"type":"directory","source_path":"/data/assets","target_path":"/data/assets"}]}"#;
        compile_test(input, &output, false);
    }

    #[test]
    fn test_invalid_json() {
        let tmp_dir = TempDir::new().unwrap();
        let tmp_in_path = tmp_dir.path().join("test.cml");
        let tmp_out_path = tmp_dir.path().join("test.cm");

        let input = json!({
            "expose": [
                { "directory": "/volumes/blobfs", "from": "realm" }
            ]
        });
        File::create(&tmp_in_path).unwrap().write_all(format!("{}", input).as_bytes()).unwrap();
        {
            let result = compile(&tmp_in_path, false, Some(tmp_out_path.clone()));
            let expected_result: Result<(), Error> =
                Err(Error::parse("Pattern condition is not met at /expose/0/from"));
            assert_eq!(format!("{:?}", result), format!("{:?}", expected_result));
        }
        // Compilation failed so output should not exist.
        {
            let result = fs::File::open(&tmp_out_path);
            assert_eq!(result.unwrap_err().kind(), io::ErrorKind::NotFound);
        }
    }
}
