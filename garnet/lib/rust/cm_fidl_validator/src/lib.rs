// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_sys2::{
        ChildDecl, ComponentDecl, ExposeDecl, OfferDecl, OfferTarget, Relation, RelativeId,
    },
    lazy_static::lazy_static,
    regex::Regex,
    std::collections::{HashMap, HashSet},
    std::error,
    std::fmt,
};

lazy_static! {
    static ref PATH: Identifier = Identifier::new(r"^(/[^/]+)+$", 1024);
    static ref NAME: Identifier = Identifier::new(r"^[0-9a-z_\-\.]+$", 100);
    static ref URI: Identifier = Identifier::new(r"^[0-9a-z\+\-\.]+://.+$", 4096);
}

/// Enum type that can represent any error encountered during validation.
#[derive(Debug)]
pub enum Error {
    MissingField(String, String),
    EmptyField(String, String),
    DuplicateField(String, String, String),
    InvalidField(String, String),
    FieldTooLong(String, String),
    InvalidChild(String, String),
    RelativeIdMissingChild(),
    RelativeIdExtraneousChild(String),
}

impl Error {
    pub fn missing_field(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::MissingField(decl_type.into(), keyword.into())
    }

    pub fn empty_field(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::EmptyField(decl_type.into(), keyword.into())
    }

    pub fn duplicate_field(
        decl_type: impl Into<String>,
        keyword: impl Into<String>,
        value: impl Into<String>,
    ) -> Self {
        Error::DuplicateField(decl_type.into(), keyword.into(), value.into())
    }

    pub fn invalid_field(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::InvalidField(decl_type.into(), keyword.into())
    }

    pub fn field_too_long(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::FieldTooLong(decl_type.into(), keyword.into())
    }

    pub fn invalid_child(decl_type: impl Into<String>, child: impl Into<String>) -> Self {
        Error::InvalidChild(decl_type.into(), child.into())
    }

    pub fn relative_id_missing_child() -> Self {
        Error::RelativeIdMissingChild()
    }

    pub fn relative_id_extraneous_child(child: impl Into<String>) -> Self {
        Error::RelativeIdExtraneousChild(child.into())
    }
}

impl error::Error for Error {}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match &self {
            Error::MissingField(d, k) => write!(f, "{} missing {}", d, k),
            Error::EmptyField(d, k) => write!(f, "{} has empty {}", d, k),
            Error::DuplicateField(d, k, v) => write!(f, "\"{}\" is a duplicate {} {}", v, d, k),
            Error::InvalidField(d, k) => write!(f, "{} has invalid {}", d, k),
            Error::FieldTooLong(d, k) => write!(f, "{}'s {} is too long", d, k),
            Error::InvalidChild(d, c) => {
                write!(f, "\"{}\" is referenced in {} but it does not appear in children", c, d)
            }
            Error::RelativeIdMissingChild() => {
                write!(f, "RelativeId is CHILD but missing child_name")
            }
            Error::RelativeIdExtraneousChild(c) => {
                write!(f, "RelativeId is not CHILD but has child_name \"{}\"", c)
            }
        }
    }
}

/// Represents a list of errors encountered during validation.
#[derive(Debug)]
pub struct ErrorList {
    errs: Vec<Error>,
}

impl ErrorList {
    fn new(errs: Vec<Error>) -> ErrorList {
        ErrorList { errs }
    }
}

impl error::Error for ErrorList {}

impl fmt::Display for ErrorList {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let strs: Vec<String> = self.errs.iter().map(|e| format!("{}", e)).collect();
        write!(f, "{}", strs.join(", "))
    }
}

/// Validates a ComponentDecl.
/// The ComponentDecl may ultimately originate from a CM file, or be directly constructed by the
/// caller. Either way, a ComponentDecl should always be validated before it's used. Examples
/// of what is validated (which may evolve in the future):
/// - That all semantically required fields are present
/// - That a child_name referenced in a RelativeId actually exists in the list of children
/// - That there are no duplicate target ptahs
pub fn validate(decl: &ComponentDecl) -> Result<(), ErrorList> {
    let ctx = ValidationContext { decl, all_children: HashSet::new(), errors: vec![] };
    ctx.validate().map_err(|errs| ErrorList::new(errs))
}

struct ValidationContext<'a> {
    decl: &'a ComponentDecl,
    all_children: HashSet<&'a str>,
    errors: Vec<Error>,
}

type PathMap<'a> = HashMap<String, HashSet<&'a str>>;

impl<'a> ValidationContext<'a> {
    fn validate(mut self) -> Result<(), Vec<Error>> {
        // Validate "children" and build the set of all children.
        if let Some(children) = self.decl.children.as_ref() {
            for child in children.iter() {
                self.validate_child(&child);
            }
        }

        // Validate "uses".
        if let Some(uses) = self.decl.uses.as_ref() {
            for use_ in uses.iter() {
                if use_.type_.is_none() {
                    self.errors.push(Error::missing_field("UseDecl", "type"));
                }
                PATH.check(use_.source_path.as_ref(), "UseDecl", "source_path", &mut self.errors);
                PATH.check(use_.target_path.as_ref(), "UseDecl", "target_path", &mut self.errors);
            }
        }

        // Validate "exposes".
        if let Some(exposes) = self.decl.exposes.as_ref() {
            let mut target_paths = HashSet::new();
            for expose in exposes.iter() {
                self.validate_expose(&expose, &mut target_paths);
            }
        }

        // Validate "offers".
        if let Some(offers) = self.decl.offers.as_ref() {
            let mut target_paths = HashMap::new();
            for offer in offers.iter() {
                self.validate_offer(&offer, &mut target_paths);
            }
        }

        if self.errors.is_empty() {
            Ok(())
        } else {
            Err(self.errors)
        }
    }

    fn validate_child(&mut self, child: &'a ChildDecl) {
        let name = child.name.as_ref();
        if NAME.check(name, "ChildDecl", "name", &mut self.errors) {
            let name: &str = name.unwrap();
            if !self.all_children.insert(name) {
                self.errors.push(Error::duplicate_field("ChildDecl", "name", name));
            }
        }
        URI.check(child.uri.as_ref(), "ChildDecl", "uri", &mut self.errors);
        if child.startup.is_none() {
            self.errors.push(Error::missing_field("ChildDecl", "startup"));
        }
    }

    fn validate_expose(
        &mut self,
        expose: &'a ExposeDecl,
        prev_target_paths: &mut HashSet<&'a str>,
    ) {
        if expose.type_.is_none() {
            self.errors.push(Error::missing_field("ExposeDecl", "type"));
        }
        PATH.check(expose.source_path.as_ref(), "ExposeDecl", "source_path", &mut self.errors);
        if expose.source.is_none() {
            self.errors.push(Error::missing_field("ExposeDecl", "source"));
        } else if let Ok((relation, child_name)) =
            self.extract_relative_id(expose.source.as_ref().unwrap())
        {
            match relation {
                Relation::Myself => {}
                Relation::Child => {}
                _ => {
                    self.errors.push(Error::invalid_field("ExposeDecl source", "relative_id"));
                }
            };
            if let Some(child) = child_name {
                let child: &str = child;
                if !self.all_children.contains(child) {
                    self.errors.push(Error::invalid_child("ExposeDecl source", child));
                }
            }
        }
        let target_path = expose.target_path.as_ref();
        if PATH.check(target_path, "ExposeDecl", "target_path", &mut self.errors) {
            let target_path: &str = target_path.unwrap();
            if !prev_target_paths.insert(target_path) {
                self.errors.push(Error::duplicate_field("ExposeDecl", "target_path", target_path));
            }
        }
    }

    fn validate_offer(&mut self, offer: &'a OfferDecl, prev_target_paths: &mut PathMap<'a>) {
        if offer.type_.is_none() {
            self.errors.push(Error::missing_field("OfferDecl", "type"));
        }
        PATH.check(offer.source_path.as_ref(), "OfferDecl", "source_path", &mut self.errors);
        if offer.source.is_none() {
            self.errors.push(Error::missing_field("OfferDecl", "source"));
        } else if let Ok((_, child_name)) = self.extract_relative_id(offer.source.as_ref().unwrap())
        {
            if let Some(child) = child_name {
                let child: &str = child;
                if !self.all_children.contains(child) {
                    self.errors.push(Error::invalid_child("OfferDecl source", child));
                }
            }
        }
        if offer.targets.is_none() {
            self.errors.push(Error::missing_field("OfferDecl", "targets"));
        } else {
            let targets = offer.targets.as_ref().unwrap();
            self.validate_targets(targets, prev_target_paths);
        }
    }

    fn validate_targets(
        &mut self,
        targets: &'a Vec<OfferTarget>,
        prev_target_paths: &mut PathMap<'a>,
    ) {
        if targets.is_empty() {
            self.errors.push(Error::empty_field("OfferDecl", "targets"));
        }
        for target in targets.iter() {
            let mut valid = true;
            valid &= PATH.check(
                target.target_path.as_ref(),
                "OfferTarget",
                "target_path",
                &mut self.errors,
            );
            valid &= NAME.check(
                target.child_name.as_ref(),
                "OfferTarget",
                "child_name",
                &mut self.errors,
            );
            if valid {
                let target_path: &str = target.target_path.as_ref().unwrap();
                let child_name: &str = target.child_name.as_ref().unwrap();
                if !self.all_children.contains(child_name) {
                    self.errors.push(Error::invalid_child("OfferTarget", child_name));
                }
                let paths_for_target =
                    prev_target_paths.entry(child_name.to_string()).or_insert(HashSet::new());
                if !paths_for_target.insert(target_path) {
                    self.errors.push(Error::duplicate_field(
                        "OfferDecl",
                        "target_path",
                        target_path,
                    ));
                }
            }
        }
    }

    /// extract_relative_id() returns Err(()) and pushes any errors onto |self.errors| on failure.
    fn extract_relative_id(
        &mut self,
        relative_id: &'a RelativeId,
    ) -> Result<(Relation, Option<&'a String>), ()> {
        if relative_id.relation.is_none() {
            self.errors.push(Error::missing_field("RelativeId", "relation"));
            return Err(());
        }
        match relative_id.relation.unwrap() {
            Relation::Child => {
                if relative_id.child_name.is_none() {
                    self.errors.push(Error::relative_id_missing_child());
                    return Err(());
                } else {
                    if !NAME.check(
                        relative_id.child_name.as_ref(),
                        "RelativeId",
                        "child_name",
                        &mut self.errors,
                    ) {
                        return Err(());
                    }
                }
                Ok((Relation::Child, Some(relative_id.child_name.as_ref().unwrap())))
            }
            r => {
                if relative_id.child_name.is_some() {
                    let child_name: &str = relative_id.child_name.as_ref().unwrap();
                    self.errors.push(Error::relative_id_extraneous_child(child_name));
                    return Err(());
                }
                Ok((r, None))
            }
        }
    }
}

struct Identifier {
    re: Regex,
    max_len: usize,
}

impl Identifier {
    fn new(regex: &str, max_len: usize) -> Identifier {
        Identifier { re: Regex::new(regex).unwrap(), max_len }
    }

    fn check(
        &self,
        prop: Option<&String>,
        decl_type: &str,
        keyword: &str,
        errors: &mut Vec<Error>,
    ) -> bool {
        let mut valid = true;
        if prop.is_none() {
            errors.push(Error::missing_field(decl_type, keyword));
            valid = false;
        } else {
            if !self.re.is_match(prop.unwrap()) {
                errors.push(Error::invalid_field(decl_type, keyword));
                valid = false;
            }
            if prop.unwrap().len() > self.max_len {
                errors.push(Error::field_too_long(decl_type, keyword));
                valid = false;
            }
        }
        valid
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl_fuchsia_sys2::{
            CapabilityType, ChildDecl, ComponentDecl, ExposeDecl, OfferDecl, OfferTarget, Relation,
            RelativeId, StartupMode, UseDecl,
        },
    };

    fn validate_test(input: ComponentDecl, expected_res: Result<(), ErrorList>) {
        let res = validate(&input);
        assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
    }

    fn identifier_test(identifier: &Identifier, input: &str, expected_res: Result<(), ErrorList>) {
        let mut errors = vec![];
        let res: Result<(), ErrorList> =
            match identifier.check(Some(&input.to_string()), "FooDecl", "foo", &mut errors) {
                true => Ok(()),
                false => Err(ErrorList::new(errors)),
            };
        assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
    }

    fn new_component_decl() -> ComponentDecl {
        ComponentDecl {
            program: None,
            uses: None,
            exposes: None,
            offers: None,
            facets: None,
            children: None,
        }
    }

    #[test]
    fn test_errors() {
        assert_eq!(format!("{}", Error::missing_field("Decl", "keyword")), "Decl missing keyword");
        assert_eq!(format!("{}", Error::empty_field("Decl", "keyword")), "Decl has empty keyword");
        assert_eq!(
            format!("{}", Error::duplicate_field("Decl", "keyword", "foo")),
            "\"foo\" is a duplicate Decl keyword"
        );
        assert_eq!(
            format!("{}", Error::invalid_field("Decl", "keyword")),
            "Decl has invalid keyword"
        );
        assert_eq!(
            format!("{}", Error::field_too_long("Decl", "keyword")),
            "Decl's keyword is too long"
        );
        assert_eq!(
            format!("{}", Error::invalid_child("Decl", "child")),
            "\"child\" is referenced in Decl but it does not appear in children"
        );
        assert_eq!(
            format!("{}", Error::relative_id_missing_child()),
            "RelativeId is CHILD but missing child_name"
        );
        assert_eq!(
            format!("{}", Error::relative_id_extraneous_child("child")),
            "RelativeId is not CHILD but has child_name \"child\""
        );
    }

    macro_rules! test_validate {
        (
            $(
                $test_name:ident => {
                    input = $input:expr,
                    result = $result:expr,
                },
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    validate_test($input, $result);
                }
            )+
        }
    }

    macro_rules! test_identifier {
        (
            $(
                $test_name:ident => {
                    identifier = $identifier:expr,
                    input = $input:expr,
                    result = $result:expr,
                },
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    identifier_test($identifier, $input, $result);
                }
            )+
        }
    }

    test_identifier! {
        // path
        test_identifier_path_valid => {
            identifier = &PATH,
            input = "/foo/bar",
            result = Ok(()),
        },
        test_identifier_path_invalid_empty => {
            identifier = &PATH,
            input = "",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_path_invalid_root => {
            identifier = &PATH,
            input = "/",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_path_invalid_relative => {
            identifier = &PATH,
            input = "foo/bar",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_path_invalid_trailing => {
            identifier = &PATH,
            input = "/foo/bar/",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_path_too_long => {
            identifier = &PATH,
            input = &format!("/{}", "a".repeat(1024)),
            result = Err(ErrorList::new(vec![Error::field_too_long("FooDecl", "foo")])),
        },

        // name
        test_identifier_name_valid => {
            identifier = &NAME,
            input = "abcdefghijklmnopqrstuvwxyz0123456789_-.",
            result = Ok(()),
        },
        test_identifier_name_invalid => {
            identifier = &NAME,
            input = "^bad",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_name_too_long => {
            identifier = &NAME,
            input = &format!("{}", "a".repeat(101)),
            result = Err(ErrorList::new(vec![Error::field_too_long("FooDecl", "foo")])),
        },

        // uri
        test_identifier_uri_valid => {
            identifier = &URI,
            input = "my+awesome-scheme.2://abc123!@#$%.com",
            result = Ok(()),
        },
        test_identifier_uri_invalid => {
            identifier = &URI,
            input = "fuchsia-pkg://",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_uri_too_long => {
            identifier = &URI,
            input = &format!("fuchsia-pkg://{}", "a".repeat(4083)),
            result = Err(ErrorList::new(vec![Error::field_too_long("FooDecl", "foo")])),
        },
    }

    test_validate! {
        // uses
        test_validate_uses_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.uses = Some(vec![UseDecl{
                    type_: None,
                    source_path: None,
                    target_path: None,
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("UseDecl", "type"),
                Error::missing_field("UseDecl", "source_path"),
                Error::missing_field("UseDecl", "target_path"),
            ])),
        },
        test_validate_uses_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.uses = Some(vec![UseDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some("foo/".to_string()),
                    target_path: Some("/".to_string()),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("UseDecl", "source_path"),
                Error::invalid_field("UseDecl", "target_path"),
            ])),
        },
        test_validate_uses_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.uses = Some(vec![UseDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some(format!("/{}", "a".repeat(1024))),
                    target_path: Some(format!("/{}", "b".repeat(1024))),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("UseDecl", "source_path"),
                Error::field_too_long("UseDecl", "target_path"),
            ])),
        },

        // exposes
        test_validate_exposes_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![ExposeDecl{
                    type_: None,
                    source_path: None,
                    source: None,
                    target_path: None,
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("ExposeDecl", "type"),
                Error::missing_field("ExposeDecl", "source_path"),
                Error::missing_field("ExposeDecl", "source"),
                Error::missing_field("ExposeDecl", "target_path"),
            ])),
        },
        test_validate_exposes_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![ExposeDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some("foo/".to_string()),
                    source: Some(RelativeId{
                        relation: Some(Relation::Child),
                        child_name: Some("^bad".to_string()),
                    }),
                    target_path: Some("/".to_string()),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("ExposeDecl", "source_path"),
                Error::invalid_field("RelativeId", "child_name"),
                Error::invalid_field("ExposeDecl", "target_path"),
            ])),
        },
        test_validate_exposes_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![ExposeDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some(format!("/{}", "a".repeat(1024))),
                    source: Some(RelativeId{
                        relation: Some(Relation::Child),
                        child_name: Some("b".repeat(101)),
                    }),
                    target_path: Some(format!("/{}", "b".repeat(1024))),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("ExposeDecl", "source_path"),
                Error::field_too_long("RelativeId", "child_name"),
                Error::field_too_long("ExposeDecl", "target_path"),
            ])),
        },
        test_validate_exposes_invalid_relation => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        source: Some(RelativeId{
                            relation: Some(Relation::Realm),
                            child_name: None,
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("ExposeDecl source", "relative_id"),
            ])),
        },
        test_validate_exposes_invalid_child => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        source: Some(RelativeId{
                            relation: Some(Relation::Child),
                            child_name: Some("netstack".to_string()),
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_child("ExposeDecl source", "netstack"),
            ])),
        },
        test_validate_exposes_duplicate_target => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/svc/logger".to_string()),
                        source: Some(RelativeId{
                            relation: Some(Relation::Myself),
                            child_name: None,
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                    ExposeDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/svc/logger2".to_string()),
                        source: Some(RelativeId{
                            relation: Some(Relation::Myself),
                            child_name: None,
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::duplicate_field("ExposeDecl", "target_path", "/svc/fuchsia.logger.Log"),
            ])),
        },

        // offers
        test_validate_offers_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![OfferDecl{
                    type_: None,
                    source_path: None,
                    source: None,
                    targets: None,
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("OfferDecl", "type"),
                Error::missing_field("OfferDecl", "source_path"),
                Error::missing_field("OfferDecl", "source"),
                Error::missing_field("OfferDecl", "targets"),
            ])),
        },
        test_validate_offers_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![OfferDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some(format!("/{}", "a".repeat(1024))),
                    source: Some(RelativeId{
                        relation: Some(Relation::Child),
                        child_name: Some("a".repeat(101)),
                    }),
                    targets: Some(vec![
                        OfferTarget{
                            target_path: Some(format!("/{}", "b".repeat(1024))),
                            child_name: Some("b".repeat(101)),
                        },
                    ]),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("OfferDecl", "source_path"),
                Error::field_too_long("RelativeId", "child_name"),
                Error::field_too_long("OfferTarget", "target_path"),
                Error::field_too_long("OfferTarget", "child_name"),
            ])),
        },
        test_validate_offers_invalid_child => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        source: Some(RelativeId{
                            relation: Some(Relation::Child),
                            child_name: Some("logger".to_string()),
                        }),
                        targets: Some(vec![
                            OfferTarget{
                                target_path: Some("/data/realm_assets".to_string()),
                                child_name: Some("netstack".to_string()),
                            },
                        ]),
                    },
                ]);
                decl.children = Some(vec![
                    ChildDecl{
                        name: Some("netstack".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(StartupMode::Lazy),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_child("OfferDecl source", "logger"),
            ])),
        },
        test_validate_offer_target_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![OfferDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some("/svc/logger".to_string()),
                    source: Some(RelativeId{
                        relation: Some(Relation::Myself),
                        child_name: None,
                    }),
                    targets: Some(vec![OfferTarget{target_path: None, child_name: None}]),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("OfferTarget", "target_path"),
                Error::missing_field("OfferTarget", "child_name"),
            ])),
        },
        test_validate_offer_targets_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![OfferDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some("/svc/logger".to_string()),
                    source: Some(RelativeId{
                        relation: Some(Relation::Myself),
                        child_name: None,
                    }),
                    targets: Some(vec![]),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::empty_field("OfferDecl", "targets"),
            ])),
        },
        test_validate_offer_target_duplicate_path => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![OfferDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some("/svc/logger".to_string()),
                    source: Some(RelativeId{
                        relation: Some(Relation::Myself),
                        child_name: None,
                    }),
                    targets: Some(vec![
                        OfferTarget{
                            target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                            child_name: Some("netstack".to_string()),
                        },
                        OfferTarget{
                            target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                            child_name: Some("netstack".to_string()),
                        },
                    ]),
                }]);
                decl.children = Some(vec![
                    ChildDecl{
                        name: Some("netstack".to_string()),
                        uri: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(StartupMode::Eager),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::duplicate_field("OfferDecl", "target_path", "/svc/fuchsia.logger.Log"),
            ])),
        },
        test_validate_offer_target_invalid_child => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![OfferDecl{
                    type_: Some(CapabilityType::Service),
                    source_path: Some("/svc/logger".to_string()),
                    source: Some(RelativeId{
                        relation: Some(Relation::Myself),
                        child_name: None,
                    }),
                    targets: Some(vec![
                        OfferTarget{
                            target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                            child_name: Some("netstack".to_string()),
                        },
                    ]),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_child("OfferTarget", "netstack"),
            ])),
        },
        test_validate_relative_id_relation_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        source: Some(RelativeId{
                            relation: None,
                            child_name: None,
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("RelativeId", "relation"),
            ])),
        },
        test_validate_relative_id_child_name_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl{
                        type_: Some(CapabilityType::Service),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        source: Some(RelativeId{
                            relation: Some(Relation::Child),
                            child_name: None,
                        }),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::relative_id_missing_child(),
            ])),
        },

        // children
        test_validate_children_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.children = Some(vec![ChildDecl{
                    name: None,
                    uri: None,
                    startup: None,
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("ChildDecl", "name"),
                Error::missing_field("ChildDecl", "uri"),
                Error::missing_field("ChildDecl", "startup"),
            ])),
        },
        test_validate_children_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.children = Some(vec![ChildDecl{
                    name: Some("^bad".to_string()),
                    uri: Some("bad-scheme&://blah".to_string()),
                    startup: Some(StartupMode::Lazy),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("ChildDecl", "name"),
                Error::invalid_field("ChildDecl", "uri"),
            ])),
        },
        test_validate_children_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.children = Some(vec![ChildDecl{
                    name: Some("a".repeat(1025)),
                    uri: Some(format!("fuchsia-pkg://{}", "a".repeat(4083))),
                    startup: Some(StartupMode::Lazy),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("ChildDecl", "name"),
                Error::field_too_long("ChildDecl", "uri"),
            ])),
        },
    }
}
