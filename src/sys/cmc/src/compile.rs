// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::cml::{self, CapabilityClause};
use crate::one_or_many::{OneOrMany, OneOrManyBorrow};
use crate::validate;
use cm_json::{self, cm, Error};
use serde::ser::Serialize;
use serde_json::{
    self,
    ser::{CompactFormatter, PrettyFormatter, Serializer},
    value::Value,
    Map,
};
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::PathBuf;
use std::str::from_utf8;

/// Read in a CML file and produce the equivalent CM.
pub fn compile(file: &PathBuf, pretty: bool, output: Option<PathBuf>) -> Result<(), Error> {
    match file.extension().and_then(|e| e.to_str()) {
        Some("cml") => Ok(()),
        _ => Err(Error::invalid_args(format!(
            "Input file {:?} does not have the component manifest language extension (.cml)",
            file
        ))),
    }?;
    if let Some(ref path) = output {
        match path.extension().and_then(|e| e.to_str()) {
            Some("cm") => Ok(()),
            _ => Err(Error::invalid_args(format!(
                "Output file {:?} does not have the component manifest language extension (.cml)",
                path
            ))),
        }?;
    }

    let mut buffer = String::new();
    File::open(&file.as_path())?.read_to_string(&mut buffer)?;
    let value = cm_json::from_json5_str(&buffer)?;
    let document = validate::parse_cml(value)?;
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
    serde_json::from_slice::<cm::Document>(&res)
        .map_err(|e| Error::parse(format!("Couldn't read output as JSON: {}", e)))?;
    Ok(())
}

fn compile_cml(document: cml::Document) -> Result<cm::Document, Error> {
    let mut out = cm::Document::default();
    out.program = document.program.as_ref().map(translate_program).transpose()?;
    out.children = document.children.as_ref().map(translate_children).transpose()?;
    out.uses = document.r#use.as_ref().map(translate_use).transpose()?;
    out.exposes = document.expose.as_ref().map(translate_expose).transpose()?;
    if let Some(offer) = document.offer.as_ref() {
        let all_children = document.all_children_names().into_iter().collect();
        let all_collections = document.all_collection_names().into_iter().collect();
        out.offers = Some(translate_offer(offer, &all_children, &all_collections)?);
    }
    out.environments = document.environments.as_ref().map(translate_environments).transpose()?;
    out.collections = document.collections.as_ref().map(translate_collections).transpose()?;
    out.storage = document.storage.as_ref().map(translate_storage).transpose()?;
    out.facets = document.facets.as_ref().cloned();
    out.runners = document.runners.as_ref().map(translate_runners).transpose()?;
    out.resolvers = document.resolvers.as_ref().map(translate_resolvers).transpose()?;
    Ok(out)
}

pub fn translate_program(program: &Map<String, Value>) -> Result<Map<String, Value>, Error> {
    let mut program_out: Map<String, Value> = Map::new();
    for (k, v) in program {
        match &k[..] {
            "lifecycle" => match v {
                Value::Object(events) => {
                    for (event, subscription) in events {
                        program_out.insert(format!("lifecycle.{}", event), subscription.clone());
                    }
                }
                _ => {
                    return Err(Error::parse(format!(
                        "Unexpected entry in lifecycle section: {}",
                        v
                    )));
                }
            },
            _ => {
                let _ = program_out.insert(k.clone(), v.clone());
            }
        }
    }
    return Ok(program_out);
}

/// `use` rules consume a single capability from one source (realm|framework).
fn translate_use(use_in: &Vec<cml::Use>) -> Result<Vec<cm::Use>, Error> {
    let mut out_uses = vec![];
    for use_ in use_in {
        if let Some(p) = use_.service() {
            let source = extract_use_source(use_)?;
            let target_id = one_target_capability_id(use_, use_)?;
            out_uses.push(cm::Use::Service(cm::UseService {
                source,
                source_path: cm::Path::new(p.clone())?,
                target_path: cm::Path::new(target_id)?,
            }));
        } else if let Some(p) = use_.protocol() {
            let source = extract_use_source(use_)?;
            let target_ids =
                all_target_capability_ids(use_, use_).ok_or(Error::internal("no capability"))?;
            let source_ids = p.to_vec();
            for target_id in target_ids {
                let target_path = cm::Path::new(target_id.clone())?;
                // When multiple source paths are provided, there is no way to alias each one, so
                // source_path == target_path.
                // When one source path is provided, source_path may be aliased to a different
                // target_path, so we source_paths[0] to derive the source_path.
                let source_path = if source_ids.len() == 1 {
                    cm::Path::new(source_ids[0].clone())?
                } else {
                    target_path.clone()
                };
                out_uses.push(cm::Use::Protocol(cm::UseProtocol {
                    source: source.clone(),
                    source_path,
                    target_path,
                }));
            }
        } else if let Some(p) = use_.directory() {
            let source = extract_use_source(use_)?;
            let target_id = one_target_capability_id(use_, use_)?;
            let rights = extract_use_rights(use_)?;
            let subdir = extract_use_subdir(use_)?;
            out_uses.push(cm::Use::Directory(cm::UseDirectory {
                source,
                source_path: cm::Path::new(p.clone())?,
                target_path: cm::Path::new(target_id)?,
                rights,
                subdir,
            }));
        } else if let Some(p) = use_.storage() {
            let target_path = match all_target_capability_ids(use_, use_) {
                Some(OneOrMany::One(target_path)) => Ok(cm::Path::new(target_path).ok()),
                Some(OneOrMany::Many(_)) => {
                    Err(Error::internal(format!("expecting one capability, but multiple provided")))
                }
                None => Ok(None),
            }?;
            out_uses.push(cm::Use::Storage(cm::UseStorage {
                type_: str_to_storage_type(p.as_str())?,
                target_path,
            }));
        } else if let Some(p) = use_.runner() {
            out_uses.push(cm::Use::Runner(cm::UseRunner { source_name: cm::Name::new(p.clone())? }))
        } else if let Some(p) = use_.event() {
            let source = extract_use_event_source(use_)?;
            let target_ids =
                all_target_capability_ids(use_, use_).ok_or(Error::internal("no capability"))?;
            let source_ids = p.to_vec();
            for target_id in target_ids {
                let target_name = cm::Name::new(target_id.clone())?;
                // When multiple source names are provided, there is no way to alias each one, so
                // source_name == target_name,
                // When one source name is provided, source_name may be aliased to a different
                // target_name, so we use source_names[0] to derive the source_name.
                let source_name = if source_ids.len() == 1 {
                    cm::Name::new(source_ids[0].clone())?
                } else {
                    target_name.clone()
                };
                out_uses.push(cm::Use::Event(cm::UseEvent {
                    source: source.clone(),
                    source_name,
                    target_name,
                    // We have already validated that none will be present if we were using many
                    // events.
                    filter: use_.filter.clone(),
                }));
            }
        } else {
            return Err(Error::internal(format!("no capability in use declaration")));
        };
    }
    Ok(out_uses)
}

/// `expose` rules route a single capability from one or more sources (self|framework|#<child>) to one or
/// more targets (realm|framework).
fn translate_expose(expose_in: &Vec<cml::Expose>) -> Result<Vec<cm::Expose>, Error> {
    let mut out_exposes = vec![];
    for expose in expose_in.iter() {
        let target = extract_expose_target(expose)?;
        if let Some(p) = expose.service() {
            let sources = extract_all_expose_sources(expose)?;
            let target_id = one_target_capability_id(expose, expose)?;
            for source in sources {
                out_exposes.push(cm::Expose::Service(cm::ExposeService {
                    source,
                    source_path: cm::Path::new(p.clone())?,
                    target_path: cm::Path::new(target_id.clone())?,
                    target: target.clone(),
                }))
            }
        } else if let Some(p) = expose.protocol() {
            let source = extract_single_expose_source(expose)?;
            let source_ids = p.to_vec();
            let target_ids = all_target_capability_ids(expose, expose)
                .ok_or(Error::internal("no capability"))?;
            for target_id in target_ids {
                let target_path = cm::Path::new(target_id)?;
                // When multiple source paths are provided, there is no way to alias each one, so
                // source_path == target_path.
                // When one source path is provided, source_path may be aliased to a different
                // target_path, so we source_paths[0] to derive the source_path.
                let source_path = if source_ids.len() == 1 {
                    cm::Path::new(source_ids[0].clone())?
                } else {
                    target_path.clone()
                };
                out_exposes.push(cm::Expose::Protocol(cm::ExposeProtocol {
                    source: source.clone(),
                    source_path,
                    target_path,
                    target: target.clone(),
                }))
            }
        } else if let Some(p) = expose.directory() {
            let source = extract_single_expose_source(expose)?;
            let target_id = one_target_capability_id(expose, expose)?;
            let rights = extract_expose_rights(expose)?;
            let subdir = extract_expose_subdir(expose)?;
            out_exposes.push(cm::Expose::Directory(cm::ExposeDirectory {
                source,
                source_path: cm::Path::new(p.clone())?,
                target_path: cm::Path::new(target_id)?,
                target,
                rights,
                subdir,
            }))
        } else if let Some(p) = expose.runner() {
            let source = extract_single_expose_source(expose)?;
            let target_id = one_target_capability_id(expose, expose)?;
            out_exposes.push(cm::Expose::Runner(cm::ExposeRunner {
                source,
                source_name: cm::Name::new(p.clone())?,
                target,
                target_name: cm::Name::new(target_id)?,
            }))
        } else if let Some(p) = expose.resolver() {
            let source = extract_single_expose_source(expose)?;
            let target_id = one_target_capability_id(expose, expose)?;
            out_exposes.push(cm::Expose::Resolver(cm::ExposeResolver {
                source,
                source_name: cm::Name::new(p.to_string())?,
                target,
                target_name: cm::Name::new(target_id)?,
            }))
        } else {
            return Err(Error::internal(format!("expose: must specify a known capability")));
        }
    }
    Ok(out_exposes)
}

fn dependency_type_from_string(s: &Option<String>) -> Result<cm::DependencyType, Error> {
    match s.as_ref().map(|s| s.as_str()) {
        Some("weak_for_migration") => Ok(cm::DependencyType::WeakForMigration),
        None | Some("strong") => Ok(cm::DependencyType::Strong),
        Some(x) => Err(Error::internal(format!("offer: unknown dependency {}", x))),
    }
}

/// `offer` rules route multiple capabilities from multiple sources to multiple targets.
fn translate_offer(
    offer_in: &Vec<cml::Offer>,
    all_children: &HashSet<&cml::Name>,
    all_collections: &HashSet<&cml::Name>,
) -> Result<Vec<cm::Offer>, Error> {
    let mut out_offers = vec![];
    for offer in offer_in.iter() {
        if let Some(p) = offer.service() {
            let sources = extract_all_offer_sources(offer)?;
            let targets = extract_all_targets_for_each_child(offer, all_children, all_collections)?;
            for (target, target_id) in targets {
                for source in &sources {
                    out_offers.push(cm::Offer::Service(cm::OfferService {
                        source_path: cm::Path::new(p.clone())?,
                        source: source.clone(),
                        target: target.clone(),
                        target_path: cm::Path::new(target_id.clone())?,
                    }));
                }
            }
        } else if let Some(p) = offer.protocol() {
            let source = extract_single_offer_source(offer)?;
            let targets = extract_all_targets_for_each_child(offer, all_children, all_collections)?;
            let source_ids = p.to_vec();
            for (target, target_id) in targets {
                // When multiple source paths are provided, there is no way to alias each one, so
                // source_path == target_path.
                // When one source path is provided, source_path may be aliased to a different
                // target_path, so we source_ids[0] to derive the source_path.
                let source_path = if source_ids.len() == 1 {
                    cm::Path::new(source_ids[0].clone())?
                } else {
                    cm::Path::new(target_id.clone())?
                };
                out_offers.push(cm::Offer::Protocol(cm::OfferProtocol {
                    source_path,
                    source: source.clone(),
                    target,
                    target_path: cm::Path::new(target_id)?,
                    dependency_type: dependency_type_from_string(&offer.dependency)?,
                }));
            }
        } else if let Some(p) = offer.directory() {
            let source = extract_single_offer_source(offer)?;
            let targets = extract_all_targets_for_each_child(offer, all_children, all_collections)?;
            for (target, target_id) in targets {
                out_offers.push(cm::Offer::Directory(cm::OfferDirectory {
                    source_path: cm::Path::new(p.clone())?,
                    source: source.clone(),
                    target,
                    target_path: cm::Path::new(target_id)?,
                    rights: extract_offer_rights(offer)?,
                    subdir: extract_offer_subdir(offer)?,
                    dependency_type: dependency_type_from_string(&offer.dependency)?,
                }));
            }
        } else if let Some(p) = offer.storage() {
            let type_ = str_to_storage_type(p.as_str())?;
            let source = extract_single_offer_storage_source(offer)?;
            let targets = extract_storage_targets(offer, all_children, all_collections)?;
            for target in targets {
                out_offers.push(cm::Offer::Storage(cm::OfferStorage {
                    type_: type_.clone(),
                    source: source.clone(),
                    target,
                }));
            }
        } else if let Some(p) = offer.runner() {
            let source = extract_single_offer_source(offer)?;
            let targets = extract_all_targets_for_each_child(offer, all_children, all_collections)?;
            for (target, target_id) in targets {
                out_offers.push(cm::Offer::Runner(cm::OfferRunner {
                    source: source.clone(),
                    source_name: cm::Name::new(p.clone())?,
                    target,
                    target_name: cm::Name::new(target_id)?,
                }));
            }
        } else if let Some(p) = offer.resolver() {
            let source = extract_single_offer_source(offer)?;
            let targets = extract_all_targets_for_each_child(offer, all_children, all_collections)?;
            for (target, target_id) in targets {
                out_offers.push(cm::Offer::Resolver(cm::OfferResolver {
                    source: source.clone(),
                    source_name: cm::Name::new(p.to_string())?,
                    target,
                    target_name: cm::Name::new(target_id)?,
                }));
            }
        } else if let Some(p) = offer.event() {
            let source = extract_single_offer_source(offer)?;
            let targets = extract_all_targets_for_each_child(offer, all_children, all_collections)?;
            let source_ids = p.to_vec();
            for (target, target_id) in targets {
                // When multiple source names are provided, there is no way to alias each one, so
                // source_name == target_name.
                // When one source name is provided, source_name may be aliased to a different
                // source_name, so we source_ids[0] to derive the source_name.
                let target_name = cm::Name::new(target_id)?;
                let source_name = if source_ids.len() == 1 {
                    cm::Name::new(source_ids[0].clone())?
                } else {
                    target_name.clone()
                };
                out_offers.push(cm::Offer::Event(cm::OfferEvent {
                    source: source.clone(),
                    source_name,
                    target,
                    target_name,
                    // We have already validated that none will be present if we were using many
                    // events.
                    filter: offer.filter.clone(),
                }));
            }
        } else {
            return Err(Error::internal(format!("no capability")));
        }
    }
    Ok(out_offers)
}

fn translate_children(children_in: &Vec<cml::Child>) -> Result<Vec<cm::Child>, Error> {
    let mut out_children = vec![];
    for child in children_in.iter() {
        let startup = match child.startup.as_ref().map(|s| s as &str) {
            Some(cml::LAZY) | None => cm::StartupMode::Lazy,
            Some(cml::EAGER) => cm::StartupMode::Eager,
            Some(_) => {
                return Err(Error::internal(format!("invalid startup")));
            }
        };
        let environment = child
            .environment
            .as_ref()
            .map(|e| match e {
                cml::Ref::Named(name) => Ok(cm::Name::new(name.to_string())?),
                _ => Err(Error::internal(format!("environment must be a named reference"))),
            })
            .transpose()?;
        out_children.push(cm::Child {
            name: cm::Name::new(child.name.to_string())?,
            url: cm::Url::new(child.url.clone())?,
            startup,
            environment: environment,
        });
    }
    Ok(out_children)
}

fn translate_collections(
    collections_in: &Vec<cml::Collection>,
) -> Result<Vec<cm::Collection>, Error> {
    let mut out_collections = vec![];
    for collection in collections_in.iter() {
        let durability = match &collection.durability as &str {
            cml::PERSISTENT => cm::Durability::Persistent,
            cml::TRANSIENT => cm::Durability::Transient,
            _ => {
                return Err(Error::internal(format!("invalid durability")));
            }
        };
        out_collections
            .push(cm::Collection { name: cm::Name::new(collection.name.to_string())?, durability });
    }
    Ok(out_collections)
}

fn translate_storage(storage_in: &Vec<cml::Storage>) -> Result<Vec<cm::Storage>, Error> {
    storage_in
        .iter()
        .map(|storage| {
            Ok(cm::Storage {
                name: cm::Name::new(storage.name.to_string())?,
                source_path: cm::Path::new(storage.path.clone())?,
                source: extract_single_offer_source(storage)?,
            })
        })
        .collect()
}

fn translate_runners(runners_in: &Vec<cml::Runner>) -> Result<Vec<cm::Runner>, Error> {
    runners_in
        .iter()
        .map(|runner| {
            Ok(cm::Runner {
                name: cm::Name::new(runner.name.to_string())?,
                source_path: cm::Path::new(runner.path.clone())?,
                source: extract_single_offer_source(runner)?,
            })
        })
        .collect()
}

fn translate_resolvers(resolvers_in: &Vec<cml::Resolver>) -> Result<Vec<cm::Resolver>, Error> {
    resolvers_in
        .iter()
        .map(|resolver| {
            Ok(cm::Resolver {
                name: cm::Name::new(resolver.name.to_string())?,
                source_path: cm::Path::new(resolver.path.clone())?,
            })
        })
        .collect()
}

fn translate_environments(envs_in: &Vec<cml::Environment>) -> Result<Vec<cm::Environment>, Error> {
    envs_in
        .iter()
        .map(|env| {
            Ok(cm::Environment {
                name: cm::Name::new(env.name.to_string())?,
                extends: match env.extends {
                    Some(cml::EnvironmentExtends::Realm) => cm::EnvironmentExtends::Realm,
                    Some(cml::EnvironmentExtends::None) => cm::EnvironmentExtends::None,
                    None => cm::EnvironmentExtends::None,
                },
                resolvers: env
                    .resolvers
                    .as_ref()
                    .map(|resolvers| {
                        resolvers
                            .iter()
                            .map(translate_resolver_registration)
                            .collect::<Result<Vec<_>, Error>>()
                    })
                    .transpose()?,
                stop_timeout_ms: env.stop_timeout_ms,
            })
        })
        .collect()
}

fn translate_resolver_registration(
    reg: &cml::ResolverRegistration,
) -> Result<cm::ResolverRegistration, Error> {
    Ok(cm::ResolverRegistration {
        resolver: cm::Name::new(reg.resolver.to_string())?,
        source: extract_single_offer_source(reg)?,
        scheme: reg
            .scheme
            .as_str()
            .parse()
            .map_err(|e| Error::internal(format!("invalid URL scheme: {}", e)))?,
    })
}

fn extract_use_source(in_obj: &cml::Use) -> Result<cm::Ref, Error> {
    match in_obj.from.as_ref() {
        Some(cml::Ref::Realm) => Ok(cm::Ref::Realm(cm::RealmRef {})),
        Some(cml::Ref::Framework) => Ok(cm::Ref::Framework(cm::FrameworkRef {})),
        Some(other) => Err(Error::internal(format!("invalid \"from\" for \"use\": {}", other))),
        None => Ok(cm::Ref::Realm(cm::RealmRef {})), // Default value.
    }
}

fn extract_use_event_source(in_obj: &cml::Use) -> Result<cm::Ref, Error> {
    match in_obj.from.as_ref() {
        Some(cml::Ref::Realm) => Ok(cm::Ref::Realm(cm::RealmRef {})),
        Some(cml::Ref::Framework) => Ok(cm::Ref::Framework(cm::FrameworkRef {})),
        Some(other) => Err(Error::internal(format!("invalid \"from\" for \"use\": {}", other))),
        None => Err(Error::internal(format!("No source \"from\" provided for \"use\""))),
    }
}

fn extract_use_rights(in_obj: &cml::Use) -> Result<cm::Rights, Error> {
    match in_obj.rights.as_ref() {
        Some(right_tokens) => match cml::parse_rights(right_tokens) {
            Ok(rights) => Ok(rights),
            _ => Err(Error::internal("Rights provided to use are not well formed.")),
        },
        None => Err(Error::internal("No use rights provided but required for used directories")),
    }
}

fn extract_use_subdir(in_obj: &cml::Use) -> Result<Option<cm::RelativePath>, Error> {
    in_obj
        .subdir
        .as_ref()
        .map(|s| cm::RelativePath::new(s.clone()))
        .transpose()
        .map_err(|e| Error::internal(format!("invalid \"subdir\" for \"use\": {}", e)))
}

fn extract_expose_subdir(in_obj: &cml::Expose) -> Result<Option<cm::RelativePath>, Error> {
    in_obj
        .subdir
        .as_ref()
        .map(|s| cm::RelativePath::new(s.clone()))
        .transpose()
        .map_err(|e| Error::internal(format!("invalid \"subdir\" for \"expose\": {}", e)))
}

fn extract_offer_subdir(in_obj: &cml::Offer) -> Result<Option<cm::RelativePath>, Error> {
    in_obj
        .subdir
        .as_ref()
        .map(|s| cm::RelativePath::new(s.clone()))
        .transpose()
        .map_err(|e| Error::internal(format!("invalid \"subdir\" for \"offer\": {}", e)))
}

fn extract_expose_rights(in_obj: &cml::Expose) -> Result<Option<cm::Rights>, Error> {
    match in_obj.rights.as_ref() {
        Some(rights_tokens) => match cml::parse_rights(rights_tokens) {
            Ok(rights) => Ok(Some(rights)),
            _ => Err(Error::internal("Rights provided to expose are not well formed.")),
        },
        None => Ok(None),
    }
}

fn expose_source_from_ref(reference: &cml::Ref) -> Result<cm::Ref, Error> {
    match reference {
        cml::Ref::Named(name) => {
            Ok(cm::Ref::Child(cm::ChildRef { name: cm::Name::new(name.to_string())? }))
        }
        cml::Ref::Framework => Ok(cm::Ref::Framework(cm::FrameworkRef {})),
        cml::Ref::Self_ => Ok(cm::Ref::Self_(cm::SelfRef {})),
        _ => Err(Error::internal(format!("invalid \"from\" for \"expose\": {}", reference))),
    }
}

fn extract_single_expose_source<T>(in_obj: &T) -> Result<cm::Ref, Error>
where
    T: cml::FromClause,
{
    match in_obj.from() {
        OneOrManyBorrow::One(reference) => expose_source_from_ref(reference),
        many => {
            return Err(Error::internal(format!(
                "multiple unexpected \"from\" clauses for \"expose\": {}",
                many
            )))
        }
    }
}

fn extract_all_expose_sources<T>(in_obj: &T) -> Result<Vec<cm::Ref>, Error>
where
    T: cml::FromClause,
{
    in_obj.from().iter().map(expose_source_from_ref).collect()
}

fn extract_offer_rights(in_obj: &cml::Offer) -> Result<Option<cm::Rights>, Error> {
    match in_obj.rights.as_ref() {
        Some(rights_tokens) => match cml::parse_rights(rights_tokens) {
            Ok(rights) => Ok(Some(rights)),
            _ => Err(Error::internal("Rights provided to offer are not well formed.")),
        },
        None => Ok(None),
    }
}

fn offer_source_from_ref(reference: &cml::Ref) -> Result<cm::Ref, Error> {
    match reference {
        cml::Ref::Named(name) => {
            Ok(cm::Ref::Child(cm::ChildRef { name: cm::Name::new(name.to_string())? }))
        }
        cml::Ref::Framework => Ok(cm::Ref::Framework(cm::FrameworkRef {})),
        cml::Ref::Realm => Ok(cm::Ref::Realm(cm::RealmRef {})),
        cml::Ref::Self_ => Ok(cm::Ref::Self_(cm::SelfRef {})),
    }
}

fn extract_single_offer_source<T>(in_obj: &T) -> Result<cm::Ref, Error>
where
    T: cml::FromClause,
{
    match in_obj.from() {
        OneOrManyBorrow::One(reference) => offer_source_from_ref(reference),
        many => {
            return Err(Error::internal(format!(
                "multiple unexpected \"from\" clauses for \"offer\": {}",
                many
            )))
        }
    }
}

fn extract_all_offer_sources<T>(in_obj: &T) -> Result<Vec<cm::Ref>, Error>
where
    T: cml::FromClause,
{
    in_obj.from().iter().map(offer_source_from_ref).collect()
}

fn extract_single_offer_storage_source<T>(in_obj: &T) -> Result<cm::Ref, Error>
where
    T: cml::FromClause,
{
    let from = in_obj.from();
    let reference = from.one().ok_or_else(|| {
        Error::internal(format!(
            "multiple unexpected \"from\" clauses for \"offer\": {}",
            in_obj.from()
        ))
    })?;
    match reference {
        cml::Ref::Realm => Ok(cm::Ref::Realm(cm::RealmRef {})),
        cml::Ref::Named(storage_name) => {
            Ok(cm::Ref::Storage(cm::StorageRef { name: cm::Name::new(storage_name.to_string())? }))
        }
        other => Err(Error::internal(format!("invalid \"from\" for \"offer\": {}", other))),
    }
}

fn translate_child_or_collection_ref(
    reference: &cml::Ref,
    all_children: &HashSet<&cml::Name>,
    all_collections: &HashSet<&cml::Name>,
) -> Result<cm::Ref, Error> {
    match reference {
        cml::Ref::Named(name) if all_children.contains(name) => {
            Ok(cm::Ref::Child(cm::ChildRef { name: cm::Name::new(name.to_string())? }))
        }
        cml::Ref::Named(name) if all_collections.contains(name) => {
            Ok(cm::Ref::Collection(cm::CollectionRef { name: cm::Name::new(name.to_string())? }))
        }
        cml::Ref::Named(_) => {
            Err(Error::internal(format!("dangling reference: \"{}\"", reference)))
        }
        _ => Err(Error::internal(format!("invalid child reference: \"{}\"", reference))),
    }
}

fn extract_storage_targets(
    in_obj: &cml::Offer,
    all_children: &HashSet<&cml::Name>,
    all_collections: &HashSet<&cml::Name>,
) -> Result<Vec<cm::Ref>, Error> {
    in_obj
        .to
        .iter()
        .map(|to| translate_child_or_collection_ref(to, all_children, all_collections))
        .collect()
}

// Return a list of (child, target capability id) expressed in the `offer`.
fn extract_all_targets_for_each_child(
    in_obj: &cml::Offer,
    all_children: &HashSet<&cml::Name>,
    all_collections: &HashSet<&cml::Name>,
) -> Result<Vec<(cm::Ref, String)>, Error> {
    let mut out_targets = vec![];

    let target_ids = all_target_capability_ids(in_obj, in_obj)
        .ok_or(Error::internal("no capability".to_string()))?;

    // Validate the "to" references.
    for to in &in_obj.to {
        for target_id in &target_ids {
            let target = translate_child_or_collection_ref(to, all_children, all_collections)?;
            out_targets.push((target, target_id.clone()))
        }
    }
    Ok(out_targets)
}

/// Return the target paths (or names) specified in the given capability.
fn all_target_capability_ids<T, U>(in_obj: &T, to_obj: &U) -> Option<OneOrMany<String>>
where
    T: cml::CapabilityClause,
    U: cml::AsClause,
{
    if let Some(as_) = to_obj.r#as() {
        // We've already validated that when `as` is specified, only 1 source id exists.
        Some(OneOrMany::One(as_.clone()))
    } else {
        if let Some(p) = in_obj.service() {
            Some(OneOrMany::One(p.clone()))
        } else if let Some(p) = in_obj.protocol() {
            Some(p.clone())
        } else if let Some(p) = in_obj.directory() {
            Some(OneOrMany::One(p.clone()))
        } else if let Some(p) = in_obj.runner() {
            Some(OneOrMany::One(p.clone()))
        } else if let Some(p) = in_obj.resolver() {
            Some(OneOrMany::One(p.to_string()))
        } else if let Some(p) = in_obj.event() {
            Some(p.clone())
        } else if let Some(type_) = in_obj.storage() {
            match type_.as_str() {
                "data" => Some(OneOrMany::One("/data".to_string())),
                "cache" => Some(OneOrMany::One("/cache".to_string())),
                _ => None,
            }
        } else {
            None
        }
    }
}

// Return the single path (or name) specified in the given capability.
fn one_target_capability_id<T, U>(in_obj: &T, to_obj: &U) -> Result<String, Error>
where
    T: cml::CapabilityClause,
    U: cml::AsClause,
{
    match all_target_capability_ids(in_obj, to_obj) {
        Some(OneOrMany::One(target_id)) => Ok(target_id),
        Some(OneOrMany::Many(_)) => {
            Err(Error::internal("expecting one capability, but multiple provided"))
        }
        _ => Err(Error::internal("expecting one capability, but none provided")),
    }
}

fn extract_expose_target(in_obj: &cml::Expose) -> Result<cm::ExposeTarget, Error> {
    match &in_obj.to {
        Some(cml::Ref::Realm) => Ok(cm::ExposeTarget::Realm),
        Some(cml::Ref::Framework) => Ok(cm::ExposeTarget::Framework),
        Some(other) => Err(Error::internal(format!("invalid exposed dest: \"{}\"", other))),
        None => Ok(cm::ExposeTarget::Realm),
    }
}

fn str_to_storage_type(s: &str) -> Result<cm::StorageType, Error> {
    match s {
        "data" => Ok(cm::StorageType::Data),
        "cache" => Ok(cm::StorageType::Cache),
        "meta" => Ok(cm::StorageType::Meta),
        t => Err(Error::internal(format!("unknown storage type: {}", t))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cm_json::{self, Error};
    use serde_json::json;
    use std::fs::File;
    use std::io;
    use std::io::{Read, Write};
    use tempfile::TempDir;

    macro_rules! test_compile {
        (
            $(
                $(#[$m:meta])*
                $test_name:ident => {
                    input = $input:expr,
                    output = $result:expr,
                },
            )+
        ) => {
            $(
                $(#[$m])*
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
    test_compile! {
        test_compile_empty => {
            input = json!({}),
            output = "{}",
        },

        test_compile_program => {
            input = json!({
                "program": {
                    "binary": "bin/app"
                },
                "use": [
                    { "runner": "elf" }
                ]
            }),
            output = r#"{
    "program": {
        "binary": "bin/app"
    },
    "uses": [
        {
            "runner": {
                "source_name": "elf"
            }
        }
    ]
}"#,
        },
        test_compile_program_with_lifecycle => {
            input = json!({
                "program": {
                    "binary": "bin/app",
                    "lifecycle": {
                        "stop_event": "notify",
                    }
                },
                "use": [
                    { "runner": "elf" }
                ]
            }),
            output = r#"{
    "program": {
        "binary": "bin/app",
        "lifecycle.stop_event": "notify"
    },
    "uses": [
        {
            "runner": {
                "source_name": "elf"
            }
        }
    ]
}"#,
        },

        test_compile_use => {
            input = json!({
                "use": [
                    { "service": "/fonts/CoolFonts", "as": "/svc/fuchsia.fonts.Provider" },
                    { "service": "/svc/fuchsia.sys2.Realm", "from": "framework" },
                    { "protocol": "/fonts/LegacyCoolFonts", "as": "/svc/fuchsia.fonts.LegacyProvider" },
                    { "protocol": "/svc/fuchsia.sys2.LegacyRealm", "from": "framework" },
                    { "directory": "/data/assets", "rights" : ["read_bytes"]},
                    {
                        "directory": "/data/config",
                        "from": "realm",
                        "rights": ["read_bytes"],
                        "subdir": "fonts",
                    },
                    { "storage": "meta" },
                    { "storage": "cache", "as": "/tmp" },
                    { "runner": "elf" },
                    { "runner": "web" },
                    { "event": "destroyed", "from": "realm" },
                    { "event": ["started", "stopped"], "from": "framework" },
                    {
                        "event": "capability_ready",
                        "as": "diagnostics",
                        "from": "realm",
                        "filter": { "path": "/diagnostics" }
                    },
                ],
            }),
            output = r#"{
    "uses": [
        {
            "service": {
                "source": {
                    "realm": {}
                },
                "source_path": "/fonts/CoolFonts",
                "target_path": "/svc/fuchsia.fonts.Provider"
            }
        },
        {
            "service": {
                "source": {
                    "framework": {}
                },
                "source_path": "/svc/fuchsia.sys2.Realm",
                "target_path": "/svc/fuchsia.sys2.Realm"
            }
        },
        {
            "protocol": {
                "source": {
                    "realm": {}
                },
                "source_path": "/fonts/LegacyCoolFonts",
                "target_path": "/svc/fuchsia.fonts.LegacyProvider"
            }
        },
        {
            "protocol": {
                "source": {
                    "framework": {}
                },
                "source_path": "/svc/fuchsia.sys2.LegacyRealm",
                "target_path": "/svc/fuchsia.sys2.LegacyRealm"
            }
        },
        {
            "directory": {
                "source": {
                    "realm": {}
                },
                "source_path": "/data/assets",
                "target_path": "/data/assets",
                "rights": [
                    "read_bytes"
                ]
            }
        },
        {
            "directory": {
                "source": {
                    "realm": {}
                },
                "source_path": "/data/config",
                "target_path": "/data/config",
                "rights": [
                    "read_bytes"
                ],
                "subdir": "fonts"
            }
        },
        {
            "storage": {
                "type": "meta"
            }
        },
        {
            "storage": {
                "type": "cache",
                "target_path": "/tmp"
            }
        },
        {
            "runner": {
                "source_name": "elf"
            }
        },
        {
            "runner": {
                "source_name": "web"
            }
        },
        {
            "event": {
                "source": {
                    "realm": {}
                },
                "source_name": "destroyed",
                "target_name": "destroyed",
                "filter": null
            }
        },
        {
            "event": {
                "source": {
                    "framework": {}
                },
                "source_name": "started",
                "target_name": "started",
                "filter": null
            }
        },
        {
            "event": {
                "source": {
                    "framework": {}
                },
                "source_name": "stopped",
                "target_name": "stopped",
                "filter": null
            }
        },
        {
            "event": {
                "source": {
                    "realm": {}
                },
                "source_name": "capability_ready",
                "target_name": "diagnostics",
                "filter": {
                    "path": "/diagnostics"
                }
            }
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
                    {
                      "service": "/svc/my.service.Service",
                      "from": ["#logger", "self"],
                    },
                    {
                      "protocol": "/loggers/fuchsia.logger.LegacyLog",
                      "from": "#logger",
                      "as": "/svc/fuchsia.logger.LegacyLog",
                      "to": "realm"
                    },
                    {
                        "protocol": [ "/A", "/B" ],
                        "from": "self",
                        "to": "realm"
                    },
                    {
                        "directory": "/volumes/blobfs/blob",
                        "from": "self",
                        "to": "framework",
                        "rights": ["r*"],
                    },
                    { "directory": "/hub", "from": "framework" },
                    { "runner": "web", "from": "self" },
                    { "runner": "web", "from": "#logger", "to": "realm", "as": "web-rename" },
                    { "resolver": "my_resolver", "from": "#logger", "to": "realm", "as": "pkg_resolver" }
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                ]
            }),
            output = r#"{
    "exposes": [
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/loggers/fuchsia.logger.Log",
                "target_path": "/svc/fuchsia.logger.Log",
                "target": "realm"
            }
        },
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/my.service.Service",
                "target_path": "/svc/my.service.Service",
                "target": "realm"
            }
        },
        {
            "service": {
                "source": {
                    "self": {}
                },
                "source_path": "/svc/my.service.Service",
                "target_path": "/svc/my.service.Service",
                "target": "realm"
            }
        },
        {
            "protocol": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/loggers/fuchsia.logger.LegacyLog",
                "target_path": "/svc/fuchsia.logger.LegacyLog",
                "target": "realm"
            }
        },
        {
            "protocol": {
                "source": {
                    "self": {}
                },
                "source_path": "/A",
                "target_path": "/A",
                "target": "realm"
            }
        },
        {
            "protocol": {
                "source": {
                    "self": {}
                },
                "source_path": "/B",
                "target_path": "/B",
                "target": "realm"
            }
        },
        {
            "directory": {
                "source": {
                    "self": {}
                },
                "source_path": "/volumes/blobfs/blob",
                "target_path": "/volumes/blobfs/blob",
                "target": "framework",
                "rights": [
                    "connect",
                    "enumerate",
                    "traverse",
                    "read_bytes",
                    "get_attributes"
                ]
            }
        },
        {
            "directory": {
                "source": {
                    "framework": {}
                },
                "source_path": "/hub",
                "target_path": "/hub",
                "target": "realm"
            }
        },
        {
            "runner": {
                "source": {
                    "self": {}
                },
                "source_name": "web",
                "target": "realm",
                "target_name": "web"
            }
        },
        {
            "runner": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_name": "web",
                "target": "realm",
                "target_name": "web-rename"
            }
        },
        {
            "resolver": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_name": "my_resolver",
                "target": "realm",
                "target_name": "pkg_resolver"
            }
        }
    ],
    "children": [
        {
            "name": "logger",
            "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
            "startup": "lazy"
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
                        "to": [ "#netstack" ]
                    },
                    {
                        "service": "/svc/fuchsia.logger.Log",
                        "from": "#logger",
                        "to": [ "#modular" ],
                        "as": "/svc/fuchsia.logger.SysLog",
                    },
                    {
                        "service": "/svc/my.service.Service",
                        "from": ["#logger", "self"],
                        "to": [ "#netstack" ]
                    },
                    {
                        "protocol": "/svc/fuchsia.logger.LegacyLog",
                        "from": "#logger",
                        "to": [ "#netstack" ],
                        "dependency": "weak_for_migration"
                    },
                    {
                        "protocol": "/svc/fuchsia.logger.LegacyLog",
                        "from": "#logger",
                        "to": [ "#modular" ],
                        "as": "/svc/fuchsia.logger.LegacySysLog",
                        "dependency": "strong"
                    },
                    {
                        "protocol": [
                            "/svc/fuchsia.setui.SetUiService",
                            "/svc/fuchsia.wlan.service.Wlan"
                        ],
                        "from": "realm",
                        "to": [ "#modular" ]
                    },
                    {
                        "directory": "/data/assets",
                        "from": "realm",
                        "to": [ "#netstack" ],
                        "dependency": "weak_for_migration"
                    },
                    {
                        "directory": "/data/assets",
                        "from": "realm",
                        "to": [ "#modular" ],
                        "as": "/data",
                        "subdir": "index/file",
                        "dependency": "strong"
                    },
                    {
                        "directory": "/hub",
                        "from": "framework",
                        "to": [ "#modular" ],
                        "as": "/hub",
                    },
                    {
                        "storage": "data",
                        "from": "#logger-storage",
                        "to": [
                            "#netstack",
                            "#modular"
                        ],
                    },
                    {
                        "runner": "web",
                        "from": "realm",
                        "to": [ "#modular" ],
                    },
                    {
                        "runner": "elf",
                        "from": "realm",
                        "to": [ "#modular" ],
                        "as": "elf-renamed",
                    },
                    {
                        "event": "destroyed",
                        "from": "framework",
                        "to": [ "#netstack"],
                        "as": "destroyed_net"
                    },
                    {
                        "event": [ "stopped", "started" ],
                        "from": "realm",
                        "to": [ "#modular" ],
                    },
                    {
                        "event": "capability_ready",
                        "from": "realm",
                        "to": [ "#netstack" ],
                        "as": "net-ready",
                        "filter": {
                            "path": [
                                "/diagnostics",
                                "/foo/bar"
                            ],
                        }
                    },
                    {
                        "resolver": "my_resolver",
                        "from": "realm",
                        "to": [ "#modular" ],
                        "as": "pkg_resolver",
                    },
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm"
                    },
                    {
                        "name": "netstack",
                        "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm"
                    },
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                ],
                "storage": [
                    {
                        "name": "logger-storage",
                        "path": "/minfs",
                        "from": "#logger",
                    },
                ],
            }),
            output = r#"{
    "offers": [
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.Log",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/svc/fuchsia.logger.Log"
            }
        },
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.Log",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/svc/fuchsia.logger.SysLog"
            }
        },
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/my.service.Service",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/svc/my.service.Service"
            }
        },
        {
            "service": {
                "source": {
                    "self": {}
                },
                "source_path": "/svc/my.service.Service",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/svc/my.service.Service"
            }
        },
        {
            "protocol": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.LegacyLog",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/svc/fuchsia.logger.LegacyLog",
                "dependency_type": "weak_for_migration"
            }
        },
        {
            "protocol": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.LegacyLog",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/svc/fuchsia.logger.LegacySysLog",
                "dependency_type": "strong"
            }
        },
        {
            "protocol": {
                "source": {
                    "realm": {}
                },
                "source_path": "/svc/fuchsia.setui.SetUiService",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/svc/fuchsia.setui.SetUiService",
                "dependency_type": "strong"
            }
        },
        {
            "protocol": {
                "source": {
                    "realm": {}
                },
                "source_path": "/svc/fuchsia.wlan.service.Wlan",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/svc/fuchsia.wlan.service.Wlan",
                "dependency_type": "strong"
            }
        },
        {
            "directory": {
                "source": {
                    "realm": {}
                },
                "source_path": "/data/assets",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/data/assets",
                "dependency_type": "weak_for_migration"
            }
        },
        {
            "directory": {
                "source": {
                    "realm": {}
                },
                "source_path": "/data/assets",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/data",
                "subdir": "index/file",
                "dependency_type": "strong"
            }
        },
        {
            "directory": {
                "source": {
                    "framework": {}
                },
                "source_path": "/hub",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/hub",
                "dependency_type": "strong"
            }
        },
        {
            "storage": {
                "type": "data",
                "source": {
                    "storage": {
                        "name": "logger-storage"
                    }
                },
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                }
            }
        },
        {
            "storage": {
                "type": "data",
                "source": {
                    "storage": {
                        "name": "logger-storage"
                    }
                },
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                }
            }
        },
        {
            "runner": {
                "source": {
                    "realm": {}
                },
                "source_name": "web",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_name": "web"
            }
        },
        {
            "runner": {
                "source": {
                    "realm": {}
                },
                "source_name": "elf",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_name": "elf-renamed"
            }
        },
        {
            "event": {
                "source": {
                    "framework": {}
                },
                "source_name": "destroyed",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_name": "destroyed_net",
                "filter": null
            }
        },
        {
            "event": {
                "source": {
                    "realm": {}
                },
                "source_name": "stopped",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_name": "stopped",
                "filter": null
            }
        },
        {
            "event": {
                "source": {
                    "realm": {}
                },
                "source_name": "started",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_name": "started",
                "filter": null
            }
        },
        {
            "event": {
                "source": {
                    "realm": {}
                },
                "source_name": "capability_ready",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_name": "net-ready",
                "filter": {
                    "path": [
                        "/diagnostics",
                        "/foo/bar"
                    ]
                }
            }
        },
        {
            "resolver": {
                "source": {
                    "realm": {}
                },
                "source_name": "my_resolver",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_name": "pkg_resolver"
            }
        }
    ],
    "children": [
        {
            "name": "logger",
            "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
            "startup": "lazy"
        },
        {
            "name": "netstack",
            "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
            "startup": "lazy"
        }
    ],
    "collections": [
        {
            "name": "modular",
            "durability": "persistent"
        }
    ],
    "storage": [
        {
            "name": "logger-storage",
            "source_path": "/minfs",
            "source": {
                "child": {
                    "name": "logger"
                }
            }
        }
    ]
}"#,
        },

        test_compile_children => {
            input = json!({
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                    },
                    {
                        "name": "gmail",
                        "url": "https://www.google.com/gmail",
                        "startup": "eager",
                    },
                    {
                        "name": "echo",
                        "url": "fuchsia-pkg://fuchsia.com/echo/stable#meta/echo.cm",
                        "startup": "lazy",
                    },
                ]
            }),
            output = r#"{
    "children": [
        {
            "name": "logger",
            "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
            "startup": "lazy"
        },
        {
            "name": "gmail",
            "url": "https://www.google.com/gmail",
            "startup": "eager"
        },
        {
            "name": "echo",
            "url": "fuchsia-pkg://fuchsia.com/echo/stable#meta/echo.cm",
            "startup": "lazy"
        }
    ]
}"#,
        },

        test_compile_collections => {
            input = json!({
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                    {
                        "name": "tests",
                        "durability": "transient",
                    },
                ]
            }),
            output = r#"{
    "collections": [
        {
            "name": "modular",
            "durability": "persistent"
        },
        {
            "name": "tests",
            "durability": "transient"
        }
    ]
}"#,
        },

        test_compile_storage => {
            input = json!({
                "storage": [
                    {
                        "name": "mystorage",
                        "path": "/storage",
                        "from": "#minfs",
                    }
                ],
                "children": [
                    {
                        "name": "minfs",
                        "url": "fuchsia-pkg://fuchsia.com/minfs/stable#meta/minfs.cm",
                    },
                ]
            }),
            output = r#"{
    "children": [
        {
            "name": "minfs",
            "url": "fuchsia-pkg://fuchsia.com/minfs/stable#meta/minfs.cm",
            "startup": "lazy"
        }
    ],
    "storage": [
        {
            "name": "mystorage",
            "source_path": "/storage",
            "source": {
                "child": {
                    "name": "minfs"
                }
            }
        }
    ]
}"#,
        },

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
            "year": 2018
        }
    }
}"#,
        },
        test_compile_runner => {
            input = json!({
                "runners": [
                    {
                        "name": "myrunner",
                        "path": "/runner",
                        "from": "self",
                    }
                ],
            }),
            output = r#"{
    "runners": [
        {
            "name": "myrunner",
            "source": {
                "self": {}
            },
            "source_path": "/runner"
        }
    ]
}"#,
        },
        test_compile_environment => {
            input = json!({
                "environments": [
                    {
                        "name": "myenv",
                    },
                    {
                        "name": "myenv2",
                        "extends": "realm",
                    },
                    {
                        "name": "myenv3",
                        "extends": "none",
                        "__stop_timeout_ms": 8000,
                    }
                ],
            }),
            output = r#"{
    "environments": [
        {
            "name": "myenv",
            "extends": "none"
        },
        {
            "name": "myenv2",
            "extends": "realm"
        },
        {
            "name": "myenv3",
            "extends": "none",
            "__stop_timeout_ms": 8000
        }
    ]
}"#,
        },
        test_compile_environment_with_resolver => {
            input = json!({
                "environments": [
                    {
                        "name": "myenv",
                        "resolvers": [
                            {
                                "resolver": "pkg_resolver",
                                "from": "realm",
                                "scheme": "fuchsia-pkg",
                            }
                        ]
                    },
                ],
            }),
            output = r#"{
    "environments": [
        {
            "name": "myenv",
            "extends": "none",
            "resolvers": [
                {
                    "resolver": "pkg_resolver",
                    "source": {
                        "realm": {}
                    },
                    "scheme": "fuchsia-pkg"
                }
            ]
        }
    ]
}"#,
        },

        test_compile_all_sections => {
            input = json!({
                "program": {
                    "binary": "bin/app",
                },
                "use": [
                    { "service": "/fonts/CoolFonts", "as": "/svc/fuchsia.fonts.Provider" },
                    { "protocol": "/fonts/LegacyCoolFonts", "as": "/svc/fuchsia.fonts.LegacyProvider" },
                    { "protocol": [ "/fonts/ReallyGoodFonts", "/fonts/IWouldNeverUseTheseFonts"]},
                    { "runner": "elf" },
                ],
                "expose": [
                    { "directory": "/volumes/blobfs", "from": "self", "rights": ["r*"]},
                ],
                "offer": [
                    {
                        "service": "/svc/fuchsia.logger.Log",
                        "from": "#logger",
                        "to": [ "#netstack", "#modular" ]
                    },
                    {
                        "protocol": "/svc/fuchsia.logger.LegacyLog",
                        "from": "#logger",
                        "to": [ "#netstack", "#modular" ],
                        "dependency_type": "strong"
                    },
                ],
                "children": [
                    {
                        "name": "logger",
                        "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
                    },
                    {
                        "name": "netstack",
                        "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
                    },
                ],
                "collections": [
                    {
                        "name": "modular",
                        "durability": "persistent",
                    },
                ],
                "facets": {
                    "author": "Fuchsia",
                    "year": 2018,
                },
                "runners": [
                    {
                        "name": "myrunner",
                        "path": "/runner",
                        "from": "self",
                    }
                ],
                "resolvers": [
                    {
                        "name": "myresolver",
                        "path": "/myresolver",
                    }
                ],
                "environments": [
                    {
                        "name": "myenv",
                        "extends": "realm"
                    }
                ],
            }),
            output = r#"{
    "program": {
        "binary": "bin/app"
    },
    "uses": [
        {
            "service": {
                "source": {
                    "realm": {}
                },
                "source_path": "/fonts/CoolFonts",
                "target_path": "/svc/fuchsia.fonts.Provider"
            }
        },
        {
            "protocol": {
                "source": {
                    "realm": {}
                },
                "source_path": "/fonts/LegacyCoolFonts",
                "target_path": "/svc/fuchsia.fonts.LegacyProvider"
            }
        },
        {
            "protocol": {
                "source": {
                    "realm": {}
                },
                "source_path": "/fonts/ReallyGoodFonts",
                "target_path": "/fonts/ReallyGoodFonts"
            }
        },
        {
            "protocol": {
                "source": {
                    "realm": {}
                },
                "source_path": "/fonts/IWouldNeverUseTheseFonts",
                "target_path": "/fonts/IWouldNeverUseTheseFonts"
            }
        },
        {
            "runner": {
                "source_name": "elf"
            }
        }
    ],
    "exposes": [
        {
            "directory": {
                "source": {
                    "self": {}
                },
                "source_path": "/volumes/blobfs",
                "target_path": "/volumes/blobfs",
                "target": "realm",
                "rights": [
                    "connect",
                    "enumerate",
                    "traverse",
                    "read_bytes",
                    "get_attributes"
                ]
            }
        }
    ],
    "offers": [
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.Log",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/svc/fuchsia.logger.Log"
            }
        },
        {
            "service": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.Log",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/svc/fuchsia.logger.Log"
            }
        },
        {
            "protocol": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.LegacyLog",
                "target": {
                    "child": {
                        "name": "netstack"
                    }
                },
                "target_path": "/svc/fuchsia.logger.LegacyLog",
                "dependency_type": "strong"
            }
        },
        {
            "protocol": {
                "source": {
                    "child": {
                        "name": "logger"
                    }
                },
                "source_path": "/svc/fuchsia.logger.LegacyLog",
                "target": {
                    "collection": {
                        "name": "modular"
                    }
                },
                "target_path": "/svc/fuchsia.logger.LegacyLog",
                "dependency_type": "strong"
            }
        }
    ],
    "children": [
        {
            "name": "logger",
            "url": "fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm",
            "startup": "lazy"
        },
        {
            "name": "netstack",
            "url": "fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm",
            "startup": "lazy"
        }
    ],
    "collections": [
        {
            "name": "modular",
            "durability": "persistent"
        }
    ],
    "facets": {
        "author": "Fuchsia",
        "year": 2018
    },
    "runners": [
        {
            "name": "myrunner",
            "source": {
                "self": {}
            },
            "source_path": "/runner"
        }
    ],
    "resolvers": [
        {
            "name": "myresolver",
            "source_path": "/myresolver"
        }
    ],
    "environments": [
        {
            "name": "myenv",
            "extends": "realm"
        }
    ]
}"#,
        },
    }

    #[test]
    fn test_compile_compact() {
        let input = json!({
            "use": [
                { "service": "/fonts/CoolFonts", "as": "/svc/fuchsia.fonts.Provider" },
                { "protocol": "/fonts/LegacyCoolFonts", "as": "/svc/fuchsia.fonts.LegacyProvider" },
                { "directory": "/data/assets", "rights": ["read_bytes"] }
            ]
        });
        let output = r#"{"uses":[{"service":{"source":{"realm":{}},"source_path":"/fonts/CoolFonts","target_path":"/svc/fuchsia.fonts.Provider"}},{"protocol":{"source":{"realm":{}},"source_path":"/fonts/LegacyCoolFonts","target_path":"/svc/fuchsia.fonts.LegacyProvider"}},{"directory":{"source":{"realm":{}},"source_path":"/data/assets","target_path":"/data/assets","rights":["read_bytes"]}}]}"#;
        compile_test(input, &output, false);
    }

    #[test]
    fn test_invalid_json() {
        use cm_json::CML_SCHEMA;

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
            let expected_result: Result<(), Error> = Err(Error::validate_schema(
                CML_SCHEMA,
                "OneOf conditions are not met at /expose/0/from",
            ));
            assert_eq!(format!("{:?}", result), format!("{:?}", expected_result));
        }
        // Compilation failed so output should not exist.
        {
            let result = fs::File::open(&tmp_out_path);
            assert_eq!(result.unwrap_err().kind(), io::ErrorKind::NotFound);
        }
    }
}
