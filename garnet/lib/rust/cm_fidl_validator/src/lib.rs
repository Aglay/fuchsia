// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_sys2 as fsys,
    lazy_static::lazy_static,
    regex::Regex,
    std::collections::{HashMap, HashSet},
    std::error,
    std::fmt,
};

lazy_static! {
    static ref PATH: Identifier = Identifier::new(r"^(/[^/]+)+$", 1024);
    static ref NAME: Identifier = Identifier::new(r"^[0-9a-z_\-\.]+$", 100);
    static ref URL: Identifier = Identifier::new(r"^[0-9a-z\+\-\.]+://.+$", 4096);
}

/// Enum type that can represent any error encountered durlng validation.
#[derive(Debug)]
pub enum Error {
    MissingField(String, String),
    EmptyField(String, String),
    ExtraneousField(String, String),
    DuplicateField(String, String, String),
    InvalidField(String, String),
    FieldTooLong(String, String),
    OfferTargetEqualsSource(String),
    InvalidChild(String, String),
    InvalidCollection(String, String),
}

impl Error {
    pub fn missing_field(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::MissingField(decl_type.into(), keyword.into())
    }

    pub fn empty_field(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::EmptyField(decl_type.into(), keyword.into())
    }

    pub fn extraneous_field(decl_type: impl Into<String>, keyword: impl Into<String>) -> Self {
        Error::ExtraneousField(decl_type.into(), keyword.into())
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

    pub fn offer_target_equals_source(decl_type: impl Into<String>) -> Self {
        Error::OfferTargetEqualsSource(decl_type.into())
    }

    pub fn invalid_child(decl_type: impl Into<String>, child: impl Into<String>) -> Self {
        Error::InvalidChild(decl_type.into(), child.into())
    }

    pub fn invalid_collection(decl_type: impl Into<String>, collection: impl Into<String>) -> Self {
        Error::InvalidCollection(decl_type.into(), collection.into())
    }
}

impl error::Error for Error {}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match &self {
            Error::MissingField(d, k) => write!(f, "{} missing {}", d, k),
            Error::EmptyField(d, k) => write!(f, "{} has empty {}", d, k),
            Error::ExtraneousField(d, k) => write!(f, "{} has extraneous {}", d, k),
            Error::DuplicateField(d, k, v) => write!(f, "\"{}\" is a duplicate {} {}", v, d, k),
            Error::InvalidField(d, k) => write!(f, "{} has invalid {}", d, k),
            Error::FieldTooLong(d, k) => write!(f, "{}'s {} is too long", d, k),
            Error::OfferTargetEqualsSource(d) => {
                write!(f, "OfferTarget \"{}\" is same as source", d)
            }
            Error::InvalidChild(d, c) => {
                write!(f, "\"{}\" is referenced in {} but it does not appear in children", c, d)
            }
            Error::InvalidCollection(d, c) => {
                write!(f, "\"{}\" is referenced in {} but it does not appear in collections", c, d)
            }
        }
    }
}

/// Represents a list of errors encountered durlng validation.
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
/// - That a child_name referenced in a source actually exists in the list of children
/// - That there are no duplicate target ptahs
pub fn validate(decl: &fsys::ComponentDecl) -> Result<(), ErrorList> {
    let ctx = ValidationContext {
        decl,
        all_children: HashSet::new(),
        all_collections: HashSet::new(),
        child_target_paths: HashMap::new(),
        collection_target_paths: HashMap::new(),
        errors: vec![],
    };
    ctx.validate().map_err(|errs| ErrorList::new(errs))
}

struct ValidationContext<'a> {
    decl: &'a fsys::ComponentDecl,
    all_children: HashSet<&'a str>,
    all_collections: HashSet<&'a str>,
    child_target_paths: PathMap<'a>,
    collection_target_paths: PathMap<'a>,
    errors: Vec<Error>,
}

type PathMap<'a> = HashMap<String, HashSet<&'a str>>;

impl<'a> ValidationContext<'a> {
    fn validate(mut self) -> Result<(), Vec<Error>> {
        // Validate "children" and build the set of all children.
        if let Some(children) = self.decl.children.as_ref() {
            for child in children.iter() {
                self.validate_child_decl(&child);
            }
        }

        // Validate "collections" and build the set of all collections.
        if let Some(collections) = self.decl.collections.as_ref() {
            for collection in collections.iter() {
                self.validate_collection_decl(&collection);
            }
        }

        // Validate "uses".
        if let Some(uses) = self.decl.uses.as_ref() {
            for use_ in uses.iter() {
                self.validate_use_decl(&use_);
            }
        }

        // Validate "exposes".
        if let Some(exposes) = self.decl.exposes.as_ref() {
            let mut target_paths = HashSet::new();
            for expose in exposes.iter() {
                self.validate_expose_decl(&expose, &mut target_paths);
            }
        }

        // Validate "offers".
        if let Some(offers) = self.decl.offers.as_ref() {
            for offer in offers.iter() {
                self.validate_offer_decl(&offer);
            }
        }

        if self.errors.is_empty() {
            Ok(())
        } else {
            Err(self.errors)
        }
    }

    fn validate_use_decl(&mut self, use_: &fsys::UseDecl) {
        match use_ {
            fsys::UseDecl::Service(u) => {
                self.validate_use_fields(
                    "UseServiceDecl",
                    u.source_path.as_ref(),
                    u.target_path.as_ref(),
                );
            }
            fsys::UseDecl::Directory(u) => {
                self.validate_use_fields(
                    "UseDirectoryDecl",
                    u.source_path.as_ref(),
                    u.target_path.as_ref(),
                );
            }
            fsys::UseDecl::__UnknownVariant { .. } => {
                self.errors.push(Error::invalid_field("ComponentDecl", "use"));
            }
        }
    }

    fn validate_use_fields(
        &mut self,
        decl: &str,
        source_path: Option<&String>,
        target_path: Option<&String>,
    ) {
        PATH.check(source_path, decl, "source_path", &mut self.errors);
        PATH.check(target_path, decl, "target_path", &mut self.errors);
    }

    fn validate_child_decl(&mut self, child: &'a fsys::ChildDecl) {
        let name = child.name.as_ref();
        if NAME.check(name, "ChildDecl", "name", &mut self.errors) {
            let name: &str = name.unwrap();
            if !self.all_children.insert(name) {
                self.errors.push(Error::duplicate_field("ChildDecl", "name", name));
            }
        }
        URL.check(child.url.as_ref(), "ChildDecl", "url", &mut self.errors);
        if child.startup.is_none() {
            self.errors.push(Error::missing_field("ChildDecl", "startup"));
        }
    }

    fn validate_collection_decl(&mut self, collection: &'a fsys::CollectionDecl) {
        let name = collection.name.as_ref();
        if NAME.check(name, "CollectionDecl", "name", &mut self.errors) {
            let name: &str = name.unwrap();
            if !self.all_collections.insert(name) {
                self.errors.push(Error::duplicate_field("CollectionDecl", "name", name));
            }
        }
        if collection.durability.is_none() {
            self.errors.push(Error::missing_field("CollectionDecl", "durability"));
        }
    }

    fn validate_source_child(&mut self, child: &fsys::ChildRef, decl_type: &str) {
        let mut valid = true;
        valid &= NAME.check(child.name.as_ref(), decl_type, "source.child.name", &mut self.errors);
        valid &= if child.collection.is_some() {
            self.errors.push(Error::extraneous_field(decl_type, "source.child.collection"));
            false
        } else {
            true
        };
        if !valid {
            return;
        }
        if let Some(child_name) = &child.name {
            if !self.all_children.contains(child_name as &str) {
                self.errors.push(Error::invalid_child(
                    format!("{} source", decl_type),
                    child_name as &str,
                ));
            }
        } else {
            self.errors.push(Error::missing_field(decl_type, "source.child.name"));
        }
    }

    fn validate_expose_decl(
        &mut self,
        expose: &'a fsys::ExposeDecl,
        prev_target_paths: &mut HashSet<&'a str>,
    ) {
        match expose {
            fsys::ExposeDecl::Service(e) => {
                self.validate_expose_fields(
                    "ExposeServiceDecl",
                    e.source.as_ref(),
                    e.source_path.as_ref(),
                    e.target_path.as_ref(),
                    prev_target_paths,
                );
            }
            fsys::ExposeDecl::Directory(e) => {
                self.validate_expose_fields(
                    "ExposeDirectoryDecl",
                    e.source.as_ref(),
                    e.source_path.as_ref(),
                    e.target_path.as_ref(),
                    prev_target_paths,
                );
            }
            fsys::ExposeDecl::__UnknownVariant { .. } => {
                self.errors.push(Error::invalid_field("ComponentDecl", "expose"));
            }
        }
    }

    fn validate_expose_fields(
        &mut self,
        decl: &str,
        source: Option<&fsys::ExposeSource>,
        source_path: Option<&String>,
        target_path: Option<&'a String>,
        prev_child_target_paths: &mut HashSet<&'a str>,
    ) {
        match source {
            Some(r) => match r {
                fsys::ExposeSource::Myself(_) => {}
                fsys::ExposeSource::Child(child) => {
                    self.validate_source_child(child, decl);
                }
                fsys::ExposeSource::__UnknownVariant { .. } => {
                    self.errors.push(Error::invalid_field(decl, "source"));
                }
            },
            None => {
                self.errors.push(Error::missing_field(decl, "source"));
            }
        }
        PATH.check(source_path, decl, "source_path", &mut self.errors);
        if PATH.check(target_path, decl, "target_path", &mut self.errors) {
            let target_path: &str = target_path.unwrap();
            if !prev_child_target_paths.insert(target_path) {
                self.errors.push(Error::duplicate_field(decl, "target_path", target_path));
            }
        }
    }

    fn validate_offer_decl(&mut self, offer: &'a fsys::OfferDecl) {
        match offer {
            fsys::OfferDecl::Service(o) => {
                self.validate_offer_fields(
                    "OfferServiceDecl",
                    o.source.as_ref(),
                    o.source_path.as_ref(),
                    o.targets.as_ref(),
                );
            }
            fsys::OfferDecl::Directory(o) => {
                self.validate_offer_fields(
                    "OfferDirectoryDecl",
                    o.source.as_ref(),
                    o.source_path.as_ref(),
                    o.targets.as_ref(),
                );
            }
            fsys::OfferDecl::__UnknownVariant { .. } => {
                self.errors.push(Error::invalid_field("ComponentDecl", "offer"));
            }
        }
    }

    fn validate_offer_fields(
        &mut self,
        decl: &str,
        source: Option<&fsys::OfferSource>,
        source_path: Option<&String>,
        targets: Option<&'a Vec<fsys::OfferTarget>>,
    ) {
        match source {
            Some(r) => match r {
                fsys::OfferSource::Realm(_) => {}
                fsys::OfferSource::Myself(_) => {}
                fsys::OfferSource::Child(child) => {
                    self.validate_source_child(child, decl);
                }
                fsys::OfferSource::__UnknownVariant { .. } => {
                    self.errors.push(Error::invalid_field(decl, "source"));
                }
            },
            None => {
                self.errors.push(Error::missing_field(decl, "source"));
            }
        }
        PATH.check(source_path, decl, "source_path", &mut self.errors);
        if let Some(targets) = targets {
            self.validate_targets(decl, source, targets);
        } else {
            self.errors.push(Error::missing_field(decl, "targets"));
        }
    }

    fn validate_targets(
        &mut self,
        decl: &str,
        source: Option<&fsys::OfferSource>,
        targets: &'a Vec<fsys::OfferTarget>,
    ) {
        let ValidationContext {
            child_target_paths,
            collection_target_paths,
            errors,
            all_children,
            all_collections,
            ..
        } = self;
        if targets.is_empty() {
            errors.push(Error::empty_field(decl, "targets"));
        }
        for target in targets.iter() {
            PATH.check(target.target_path.as_ref(), "OfferTarget", "target_path", errors);
            match target.dest {
                Some(_) => match target.dest.as_ref().unwrap() {
                    fsys::OfferDest::Child(c) => {
                        Self::validate_target_child(
                            c,
                            source,
                            target,
                            all_children,
                            child_target_paths,
                            errors,
                        );
                    }
                    fsys::OfferDest::Collection(c) => {
                        Self::validate_target_collection(
                            c,
                            target,
                            all_collections,
                            collection_target_paths,
                            errors,
                        );
                    }
                    fsys::OfferDest::__UnknownVariant { .. } => {
                        errors.push(Error::invalid_field(decl, "dest"));
                    }
                },
                None => {
                    errors.push(Error::missing_field("OfferTarget", "dest"));
                }
            }
        }
    }

    fn validate_target_child(
        child: &fsys::ChildRef,
        source: Option<&fsys::OfferSource>,
        target: &'a fsys::OfferTarget,
        all_children: &HashSet<&'a str>,
        prev_target_paths: &mut PathMap<'a>,
        errors: &mut Vec<Error>,
    ) {
        let mut valid = true;
        valid &= NAME.check(child.name.as_ref(), "OfferTarget", "dest.child.name", errors);
        valid &= if child.collection.is_some() {
            errors.push(Error::extraneous_field("OfferTarget", "dest.child.collection"));
            false
        } else {
            true
        };
        if !valid {
            return;
        }
        if let Some(target_path) = target.target_path.as_ref() {
            let child_name: &str = child.name.as_ref().unwrap();
            if !all_children.contains(child_name) {
                errors.push(Error::invalid_child("OfferTarget dest", child_name));
            }
            let paths_for_target =
                prev_target_paths.entry(child_name.to_string()).or_insert(HashSet::new());
            if !paths_for_target.insert(target_path) {
                errors.push(Error::duplicate_field(
                    "OfferTarget",
                    "target_path",
                    target_path as &str,
                ));
            }
            if let Some(source) = source {
                if let fsys::OfferSource::Child(source_child) = source {
                    if let Some(source_child_name) = &source_child.name {
                        if source_child_name == child_name {
                            errors
                                .push(Error::offer_target_equals_source(source_child_name as &str));
                        }
                    }
                }
            }
        }
    }

    fn validate_target_collection(
        collection: &fsys::CollectionRef,
        target: &'a fsys::OfferTarget,
        all_collections: &HashSet<&'a str>,
        prev_target_paths: &mut PathMap<'a>,
        errors: &mut Vec<Error>,
    ) {
        if !NAME.check(collection.name.as_ref(), "OfferTarget", "dest.collection.name", errors) {
            return;
        }
        if let Some(target_path) = target.target_path.as_ref() {
            let collection_name: &str = collection.name.as_ref().unwrap();
            if !all_collections.contains(collection_name) {
                errors.push(Error::invalid_collection("OfferTarget dest", collection_name));
            }
            let paths_for_target =
                prev_target_paths.entry(collection_name.to_string()).or_insert(HashSet::new());
            if !paths_for_target.insert(target_path) {
                errors.push(Error::duplicate_field(
                    "OfferTarget",
                    "target_path",
                    target_path as &str,
                ));
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
            ChildDecl, ChildRef, CollectionDecl, CollectionRef, ComponentDecl, Durability,
            ExposeDecl, ExposeDirectoryDecl, ExposeServiceDecl, ExposeSource, OfferDecl, OfferDest,
            OfferDirectoryDecl, OfferServiceDecl, OfferSource, OfferTarget, RealmRef, SelfRef,
            StartupMode, UseDecl, UseDirectoryDecl, UseServiceDecl,
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
            storage: None,
            children: None,
            collections: None,
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
            format!("{}", Error::invalid_collection("Decl", "child")),
            "\"child\" is referenced in Decl but it does not appear in collections"
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

        // url
        test_identifier_url_valid => {
            identifier = &URL,
            input = "my+awesome-scheme.2://abc123!@#$%.com",
            result = Ok(()),
        },
        test_identifier_url_invalid => {
            identifier = &URL,
            input = "fuchsia-pkg://",
            result = Err(ErrorList::new(vec![Error::invalid_field("FooDecl", "foo")])),
        },
        test_identifier_url_too_long => {
            identifier = &URL,
            input = &format!("fuchsia-pkg://{}", "a".repeat(4083)),
            result = Err(ErrorList::new(vec![Error::field_too_long("FooDecl", "foo")])),
        },
    }

    test_validate! {
        // uses
        test_validate_uses_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.uses = Some(vec![
                    UseDecl::Service(UseServiceDecl {
                        source_path: None,
                        target_path: None,
                    }),
                    UseDecl::Directory(UseDirectoryDecl {
                        source_path: None,
                        target_path: None,
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("UseServiceDecl", "source_path"),
                Error::missing_field("UseServiceDecl", "target_path"),
                Error::missing_field("UseDirectoryDecl", "source_path"),
                Error::missing_field("UseDirectoryDecl", "target_path"),
            ])),
        },
        test_validate_uses_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.uses = Some(vec![
                    UseDecl::Service(UseServiceDecl {
                        source_path: Some("foo/".to_string()),
                        target_path: Some("/".to_string()),
                    }),
                    UseDecl::Directory(UseDirectoryDecl {
                        source_path: Some("foo/".to_string()),
                        target_path: Some("/".to_string()),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("UseServiceDecl", "source_path"),
                Error::invalid_field("UseServiceDecl", "target_path"),
                Error::invalid_field("UseDirectoryDecl", "source_path"),
                Error::invalid_field("UseDirectoryDecl", "target_path"),
            ])),
        },
        test_validate_uses_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.uses = Some(vec![
                    UseDecl::Service(UseServiceDecl {
                        source_path: Some(format!("/{}", "a".repeat(1024))),
                        target_path: Some(format!("/{}", "b".repeat(1024))),
                    }),
                    UseDecl::Directory(UseDirectoryDecl {
                        source_path: Some(format!("/{}", "a".repeat(1024))),
                        target_path: Some(format!("/{}", "b".repeat(1024))),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("UseServiceDecl", "source_path"),
                Error::field_too_long("UseServiceDecl", "target_path"),
                Error::field_too_long("UseDirectoryDecl", "source_path"),
                Error::field_too_long("UseDirectoryDecl", "target_path"),
            ])),
        },

        // exposes
        test_validate_exposes_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl::Service(ExposeServiceDecl {
                        source: None,
                        source_path: None,
                        target_path: None,
                    }),
                    ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: None,
                        source_path: None,
                        target_path: None,
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("ExposeServiceDecl", "source"),
                Error::missing_field("ExposeServiceDecl", "source_path"),
                Error::missing_field("ExposeServiceDecl", "target_path"),
                Error::missing_field("ExposeDirectoryDecl", "source"),
                Error::missing_field("ExposeDirectoryDecl", "source_path"),
                Error::missing_field("ExposeDirectoryDecl", "target_path"),
            ])),
        },
        test_validate_exposes_extraneous => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl::Service(ExposeServiceDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: Some("modular".to_string()),
                        })),
                        source_path: Some("/svc/logger".to_string()),
                        target_path: Some("/svc/logger".to_string()),
                    }),
                    ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("netstack".to_string()),
                            collection: Some("modular".to_string()),
                        })),
                        source_path: Some("/data".to_string()),
                        target_path: Some("/data".to_string()),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::extraneous_field("ExposeServiceDecl", "source.child.collection"),
                Error::extraneous_field("ExposeDirectoryDecl", "source.child.collection"),
            ])),
        },
        test_validate_exposes_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl::Service(ExposeServiceDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("^bad".to_string()),
                            collection: None,
                        })),
                        source_path: Some("foo/".to_string()),
                        target_path: Some("/".to_string()),
                    }),
                    ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("^bad".to_string()),
                            collection: None,
                        })),
                        source_path: Some("foo/".to_string()),
                        target_path: Some("/".to_string()),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("ExposeServiceDecl", "source.child.name"),
                Error::invalid_field("ExposeServiceDecl", "source_path"),
                Error::invalid_field("ExposeServiceDecl", "target_path"),
                Error::invalid_field("ExposeDirectoryDecl", "source.child.name"),
                Error::invalid_field("ExposeDirectoryDecl", "source_path"),
                Error::invalid_field("ExposeDirectoryDecl", "target_path"),
            ])),
        },
        test_validate_exposes_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl::Service(ExposeServiceDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("b".repeat(101)),
                            collection: None,
                        })),
                        source_path: Some(format!("/{}", "a".repeat(1024))),
                        target_path: Some(format!("/{}", "b".repeat(1024))),
                    }),
                    ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("b".repeat(101)),
                            collection: None,
                        })),
                        source_path: Some(format!("/{}", "a".repeat(1024))),
                        target_path: Some(format!("/{}", "b".repeat(1024))),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("ExposeServiceDecl", "source.child.name"),
                Error::field_too_long("ExposeServiceDecl", "source_path"),
                Error::field_too_long("ExposeServiceDecl", "target_path"),
                Error::field_too_long("ExposeDirectoryDecl", "source.child.name"),
                Error::field_too_long("ExposeDirectoryDecl", "source_path"),
                Error::field_too_long("ExposeDirectoryDecl", "target_path"),
            ])),
        },
        test_validate_exposes_invalid_child => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl::Service(ExposeServiceDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("netstack".to_string()),
                            collection: None,
                        })),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    }),
                    ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: Some(ExposeSource::Child(ChildRef {
                            name: Some("netstack".to_string()),
                            collection: None,
                        })),
                        source_path: Some("/data/netstack".to_string()),
                        target_path: Some("/data".to_string()),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_child("ExposeServiceDecl source", "netstack"),
                Error::invalid_child("ExposeDirectoryDecl source", "netstack"),
            ])),
        },
        test_validate_exposes_duplicate_target => {
            input = {
                let mut decl = new_component_decl();
                decl.exposes = Some(vec![
                    ExposeDecl::Service(ExposeServiceDecl {
                        source: Some(ExposeSource::Myself(SelfRef{})),
                        source_path: Some("/svc/logger".to_string()),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    }),
                    ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: Some(ExposeSource::Myself(SelfRef{})),
                        source_path: Some("/svc/logger2".to_string()),
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::duplicate_field("ExposeDirectoryDecl", "target_path",
                                       "/svc/fuchsia.logger.Log"),
            ])),
        },

        // offers
        test_validate_offers_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: None,
                        source_path: None,
                        targets: None,
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: None,
                        source_path: None,
                        targets: None,
                    })
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("OfferServiceDecl", "source"),
                Error::missing_field("OfferServiceDecl", "source_path"),
                Error::missing_field("OfferServiceDecl", "targets"),
                Error::missing_field("OfferDirectoryDecl", "source"),
                Error::missing_field("OfferDirectoryDecl", "source_path"),
                Error::missing_field("OfferDirectoryDecl", "targets"),
            ])),
        },
        test_validate_offers_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("a".repeat(101)),
                            collection: None,
                        })),
                        source_path: Some(format!("/{}", "a".repeat(1024))),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some(format!("/{}", "b".repeat(1024))),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("b".repeat(101)),
                                       collection: None,
                                   }
                                )),
                            },
                            OfferTarget {
                                target_path: Some(format!("/{}", "b".repeat(1024))),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: Some("b".repeat(101)) }
                                )),
                            },
                        ]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("a".repeat(101)),
                            collection: None,
                        })),
                        source_path: Some(format!("/{}", "a".repeat(1024))),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some(format!("/{}", "b".repeat(1024))),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("b".repeat(101)),
                                       collection: None,
                                   }
                                )),
                            },
                            OfferTarget {
                                target_path: Some(format!("/{}", "b".repeat(1024))),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: Some("b".repeat(101)) }
                                )),
                            },
                        ]),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("OfferServiceDecl", "source.child.name"),
                Error::field_too_long("OfferServiceDecl", "source_path"),
                Error::field_too_long("OfferTarget", "target_path"),
                Error::field_too_long("OfferTarget", "dest.child.name"),
                Error::field_too_long("OfferTarget", "target_path"),
                Error::field_too_long("OfferTarget", "dest.collection.name"),
                Error::field_too_long("OfferDirectoryDecl", "source.child.name"),
                Error::field_too_long("OfferDirectoryDecl", "source_path"),
                Error::field_too_long("OfferTarget", "target_path"),
                Error::field_too_long("OfferTarget", "dest.child.name"),
                Error::field_too_long("OfferTarget", "target_path"),
                Error::field_too_long("OfferTarget", "dest.collection.name"),
            ])),
        },
        test_validate_offers_extraneous => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: Some("modular".to_string()),
                        })),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some("/data/realm_assets".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("netstack".to_string()),
                                       collection: Some("modular".to_string()),
                                   }
                                )),
                            },
                        ]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: Some("modular".to_string()),
                        })),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some("/data".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("netstack".to_string()),
                                       collection: Some("modular".to_string()),
                                   }
                                )),
                            },
                        ]),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::extraneous_field("OfferServiceDecl", "source.child.collection"),
                Error::extraneous_field("OfferTarget", "dest.child.collection"),
                Error::extraneous_field("OfferDirectoryDecl", "source.child.collection"),
                Error::extraneous_field("OfferTarget", "dest.child.collection"),
            ])),
        },
        test_validate_offers_invalid_child => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: None,
                        })),
                        source_path: Some("/loggers/fuchsia.logger.Log".to_string()),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some("/data/realm_assets".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("netstack".to_string()),
                                       collection: None,
                                   }
                                )),
                            },
                        ]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: None,
                        })),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some("/data".to_string()),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: Some("modular".to_string()) }
                                )),
                            },
                        ]),
                    }),
                ]);
                decl.children = Some(vec![
                    ChildDecl{
                        name: Some("netstack".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(StartupMode::Lazy),
                    },
                ]);
                decl.collections = Some(vec![
                    CollectionDecl{
                        name: Some("modular".to_string()),
                        durability: Some(Durability::Persistent),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_child("OfferServiceDecl source", "logger"),
                Error::invalid_child("OfferDirectoryDecl source", "logger"),
            ])),
        },
        test_validate_offer_target_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Realm(RealmRef{})),
                        source_path: Some("/svc/logger".to_string()),
                        targets: Some(vec![OfferTarget{target_path: None, dest: None}]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Realm(RealmRef{})),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![OfferTarget{target_path: None, dest: None}]),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("OfferTarget", "target_path"),
                Error::missing_field("OfferTarget", "dest"),
                Error::missing_field("OfferTarget", "target_path"),
                Error::missing_field("OfferTarget", "dest"),
            ])),
        },
        test_validate_offer_targets_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Myself(SelfRef{})),
                        source_path: Some("/svc/logger".to_string()),
                        targets: Some(vec![]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Myself(SelfRef{})),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![]),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::empty_field("OfferServiceDecl", "targets"),
                Error::empty_field("OfferDirectoryDecl", "targets"),
            ])),
        },
        test_validate_offer_target_equals_from => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: None,
                        })),
                        source_path: Some("/svc/logger".to_string()),
                        targets: Some(vec![OfferTarget{
                            target_path: Some("/svc/logger".to_string()),
                            dest: Some(OfferDest::Child(
                               ChildRef {
                                   name: Some("logger".to_string()),
                                   collection: None,
                               }
                            )),
                        }]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Child(ChildRef {
                            name: Some("logger".to_string()),
                            collection: None,
                        })),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![OfferTarget{
                            target_path: Some("/data".to_string()),
                            dest: Some(OfferDest::Child(
                               ChildRef {
                                   name: Some("logger".to_string()),
                                   collection: None,
                               }
                            )),
                        }]),
                    }),
                ]);
                decl.children = Some(vec![ChildDecl{
                    name: Some("logger".to_string()),
                    url: Some("fuchsia-pkg://fuchsia.com/logger#meta/logger.cm".to_string()),
                    startup: Some(StartupMode::Lazy),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::offer_target_equals_source("logger"),
                Error::offer_target_equals_source("logger"),
            ])),
        },
        test_validate_offer_target_duplicate_path => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Myself(SelfRef{})),
                        source_path: Some("/svc/logger".to_string()),
                        targets: Some(vec![
                            OfferTarget {
                                target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("netstack".to_string()),
                                       collection: None,
                                   }
                                )),
                            },
                            OfferTarget {
                                target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("netstack".to_string()),
                                       collection: None,
                                   }
                                )),
                            },
                        ]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Myself(SelfRef{})),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![
                            OfferTarget{
                                target_path: Some("/data".to_string()),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: Some("modular".to_string()) }
                                )),
                            },
                            OfferTarget{
                                target_path: Some("/data".to_string()),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: Some("modular".to_string()) }
                                )),
                            },
                        ]),
                    }),
                ]);
                decl.children = Some(vec![
                    ChildDecl{
                        name: Some("netstack".to_string()),
                        url: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
                        startup: Some(StartupMode::Eager),
                    },
                ]);
                decl.collections = Some(vec![
                    CollectionDecl{
                        name: Some("modular".to_string()),
                        durability: Some(Durability::Persistent),
                    },
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::duplicate_field("OfferTarget", "target_path", "/svc/fuchsia.logger.Log"),
                Error::duplicate_field("OfferTarget", "target_path", "/data"),
            ])),
        },
        test_validate_offer_target_invalid => {
            input = {
                let mut decl = new_component_decl();
                decl.offers = Some(vec![
                    OfferDecl::Service(OfferServiceDecl {
                        source: Some(OfferSource::Myself(SelfRef{})),
                        source_path: Some("/svc/logger".to_string()),
                        targets: Some(vec![
                            OfferTarget{
                                target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: Some("netstack".to_string()),
                                       collection: None,
                                   }
                                )),
                            },
                            OfferTarget{
                                target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: Some("modular".to_string()) }
                                )),
                            },
                        ]),
                    }),
                    OfferDecl::Directory(OfferDirectoryDecl {
                        source: Some(OfferSource::Myself(SelfRef{})),
                        source_path: Some("/data/assets".to_string()),
                        targets: Some(vec![
                            OfferTarget{
                                target_path: Some("/data".to_string()),
                                dest: Some(OfferDest::Child(
                                   ChildRef {
                                       name: None,
                                       collection: None,
                                   }
                                )),
                            },
                            OfferTarget{
                                target_path: Some("/data".to_string()),
                                dest: Some(OfferDest::Collection(
                                   CollectionRef { name: None }
                                )),
                            },
                        ]),
                    }),
                ]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_child("OfferTarget dest", "netstack"),
                Error::invalid_collection("OfferTarget dest", "modular"),
                Error::missing_field("OfferTarget", "dest.child.name"),
                Error::missing_field("OfferTarget", "dest.collection.name"),
            ])),
        },

        // children
        test_validate_children_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.children = Some(vec![ChildDecl{
                    name: None,
                    url: None,
                    startup: None,
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("ChildDecl", "name"),
                Error::missing_field("ChildDecl", "url"),
                Error::missing_field("ChildDecl", "startup"),
            ])),
        },
        test_validate_children_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.children = Some(vec![ChildDecl{
                    name: Some("^bad".to_string()),
                    url: Some("bad-scheme&://blah".to_string()),
                    startup: Some(StartupMode::Lazy),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("ChildDecl", "name"),
                Error::invalid_field("ChildDecl", "url"),
            ])),
        },
        test_validate_children_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.children = Some(vec![ChildDecl{
                    name: Some("a".repeat(1025)),
                    url: Some(format!("fuchsia-pkg://{}", "a".repeat(4083))),
                    startup: Some(StartupMode::Lazy),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("ChildDecl", "name"),
                Error::field_too_long("ChildDecl", "url"),
            ])),
        },

        // collections
        test_validate_collections_empty => {
            input = {
                let mut decl = new_component_decl();
                decl.collections = Some(vec![CollectionDecl{
                    name: None,
                    durability: None,
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::missing_field("CollectionDecl", "name"),
                Error::missing_field("CollectionDecl", "durability"),
            ])),
        },
        test_validate_collections_invalid_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.collections = Some(vec![CollectionDecl{
                    name: Some("^bad".to_string()),
                    durability: Some(Durability::Persistent),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::invalid_field("CollectionDecl", "name"),
            ])),
        },
        test_validate_collections_long_identifiers => {
            input = {
                let mut decl = new_component_decl();
                decl.collections = Some(vec![CollectionDecl{
                    name: Some("a".repeat(1025)),
                    durability: Some(Durability::Transient),
                }]);
                decl
            },
            result = Err(ErrorList::new(vec![
                Error::field_too_long("CollectionDecl", "name"),
            ])),
        },
    }
}
