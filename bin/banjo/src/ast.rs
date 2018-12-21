// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::Rule,
    failure::Fail,
    pest::iterators::{Pair, Pairs},
    serde_derive::Serialize,
    std::fmt,
    std::{collections::{HashMap, HashSet, VecDeque}, str::FromStr},
};

#[derive(Debug, Fail)]
pub enum ParseError {
    #[fail(display = "The primary namespace was already set")]
    AlreadyPrimaryNamespace,
    #[fail(display = "{} is not yet support", 0)]
    NotYetSupported(String),
    #[fail(display = "{:?} is not expected in this location", 0)]
    UnexpectedToken(Rule),
    #[fail(display = "{} was not included in the input libraries", 0)]
    UnImported(String),
    #[fail(display = "{:?} is an unknown type", 0)]
    UnrecognizedType(String),
    #[fail(display = "Failed to parse because {:?} is not an integer", 0)]
    NotAnInteger(Rule),
    #[fail(display = "Invalid dependencies: {}", 0)]
    InvalidDeps(String),
}

#[derive(PartialEq, Eq, Serialize, Default, Debug, Hash)]
pub struct Attr {
    pub key: String,
    pub val: Option<String>,
}

#[derive(PartialEq, Eq, Serialize, Default, Debug, Hash)]
pub struct Attrs(pub Vec<Attr>);
pub type Ident = String;

impl Attrs {
    #[allow(dead_code)]
    pub fn has_attributes(&self) -> bool {
        self.0.len() > 0
    }

    pub fn has_attribute(&self, key: &str) -> bool {
        self.0.iter().any(|attr| attr.key == key)
    }

    pub fn get_attribute<'a>(&'a self, key: &str) -> Option<&'a String> {
        if let Some(attr) = self.0.iter().find(|attr| attr.key == key) {
            if let Some(ref val) = attr.val {
                Some(val)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn from_pair(pair: Pair<'_, Rule>) -> Result<Attrs, ParseError> {
        let mut attrs = Attrs::default();
        let mut doc_string: Option<String> = None; // String::default();
        for inner_pair in pair.into_inner() {
            match inner_pair.as_rule() {
                Rule::doc_comment => {
                    if doc_string.is_none() {
                        doc_string = Some(String::default())
                    }
                    if let Some(ref mut doc_string) = doc_string {
                        *doc_string += inner_pair.as_str().split_at(3).1;
                    }
                }
                Rule::attribute_list => {
                    let attr_string = inner_pair.as_str().trim_start_matches('[').trim_end_matches(']');
                    let attr_pairs = attr_string.split(",");
                    for ap in attr_pairs {
                        if !ap.contains("=") {
                            attrs.0.push(Attr {
                                key: String::from(ap.trim()),
                                val: None,
                            });
                        } else {
                            let split: Vec<&str> = ap.split("=").collect();
                            attrs.0.push(Attr {
                                key: String::from(split[0].trim()),
                                val: Some(String::from(split[1])),
                            });
                        }
                    }
                }
                _ => unreachable!(),
            }
        }
        if doc_string.is_some() {
            attrs.0.push(Attr {
                key: String::from("Doc"),
                val: doc_string,
            });
        }
        Ok(attrs)
    }
}

#[derive(PartialEq, Eq, Clone, Serialize, Debug, Hash)]
pub enum Constant {
    SizedRaw(usize),
    SizedConstant(Ident),
}

impl Constant {
    pub fn from_str(string: &str) -> Result<Self, ParseError> {
        match usize::from_str(string) {
            Ok(s) => Ok(Constant::SizedRaw(s)),
            Err(_) => Ok(Constant::SizedConstant(string.to_string())),
        }
    }
}

#[derive(PartialEq, Eq, Clone, Serialize, Debug, Hash)]
pub enum HandleTy {
    Handle,
    Process,
    Thread,
    Vmo,
    Channel,
    Event,
    Port,
    Interrupt,
    Log,
    Socket,
    Resource,
    EventPair,
    Job,
    Vmar,
    Fifo,
    Guest,
    Timer,
    Bti,
    Profile,
}

#[derive(PartialEq, Eq, Clone, Serialize, Debug, Hash)]
pub enum Ty {
    Voidptr,
    USize,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float32,
    Float64,
    Str { size: Option<Constant>, nullable: bool },
    Vector { ty: Box<Ty>, size: Option<Constant>, nullable: bool },
    Array { ty: Box<Ty>, size: Constant },
    Interface,
    Struct,
    Union,
    Enum,
    Handle {ty: HandleTy, nullable: bool},
    Ident {id: String, nullable: bool},
}

impl fmt::Display for Ty {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Ty::UInt32 => write!(f, "UInt32"),
            _ => panic!("unknown type in Display conversion"),
        }
    }
}

impl Ty {
    pub fn is_primitive(&self) -> bool {
        match self {
            Ty::Ident{id, ..} => {
                if id == "zx.status" {
                    return true
                } else {
                    return false
                }
            },
            Ty::Str {..} | Ty::Vector {..} | Ty::Array {..} | Ty::Handle {..} => false,
            _ => true
        }
    }
    pub fn is_nullable(&self) -> bool {
        match self {
            Ty::Str{nullable,  ..} => *nullable,
            Ty::Vector{nullable, ..} => *nullable,
            Ty::Ident{nullable, ..} => *nullable,
            Ty::Handle{nullable, ..} => *nullable,
            _  => false,
        }
    }


    pub fn from_pair(pair: &Pair<'_, Rule>) -> Result<Self, ParseError> {
        let rule = pair.as_rule();
        match rule {
            Rule::primitive_type => match pair.as_str() {
                "usize" => Ok(Ty::USize),
                "bool" => Ok(Ty::Bool),
                "int8" => Ok(Ty::Int8),
                "int16" => Ok(Ty::Int16),
                "int32" => Ok(Ty::Int32),
                "int64" => Ok(Ty::Int64),
                "uint8" => Ok(Ty::UInt8),
                "uint16" => Ok(Ty::UInt16),
                "uint32" => Ok(Ty::UInt32),
                "uint64" => Ok(Ty::UInt64),
                "float32" => Ok(Ty::Float32),
                "float64" => Ok(Ty::Float64),
                "voidptr" => Ok(Ty::Voidptr),
                _e => Err(ParseError::UnrecognizedType(
                    pair.as_str().to_string(),
                )),
            },
            Rule::handle_type => {
                let mut ty = HandleTy::Handle;
                let mut nullable = false;
                for inner_pair in pair.clone().into_inner() {
                    match inner_pair.as_rule() {
                        Rule::handle_subtype => {
                            ty = match inner_pair.as_str() {
                                "process" => HandleTy::Process,
                                "thread" => HandleTy::Thread,
                                "vmo" => HandleTy::Vmo,
                                "channel" => HandleTy::Channel,
                                "event" => HandleTy::Event,
                                "port" => HandleTy::Port,
                                "interrupt" => HandleTy::Interrupt,
                                "log" => HandleTy::Log,
                                "socket" => HandleTy::Socket,
                                "resource" => HandleTy::Resource,
                                "eventpair" => HandleTy::EventPair,
                                "job" => HandleTy::Job,
                                "vmar" => HandleTy::Vmar,
                                "fifo" => HandleTy::Fifo,
                                "guest" => HandleTy::Guest,
                                "timer" => HandleTy::Timer,
                                "bti" => HandleTy::Bti,
                                "profile" => HandleTy::Profile,
                                _e => {
                                    return Err(ParseError::UnrecognizedType(
                                        inner_pair.as_str().to_string(),
                                    ))
                                }
                            }
                        }
                        Rule::nullable => { nullable = true; }
                        _e => {
                            return Err(ParseError::UnrecognizedType(
                                inner_pair.as_str().to_string(),
                            ))
                        }
,
                    }
                }
                Ok(Ty::Handle{ty, nullable})
            }
            Rule::integer_type => match pair.as_str() {
                "usize" => Ok(Ty::USize),
                "int8" => Ok(Ty::Int8),
                "int16" => Ok(Ty::Int16),
                "int32" => Ok(Ty::Int32),
                "int64" => Ok(Ty::Int64),
                "uint8" => Ok(Ty::UInt8),
                "uint16" => Ok(Ty::UInt16),
                "uint32" => Ok(Ty::UInt32),
                "uint64" => Ok(Ty::UInt64),
                _e => Err(ParseError::NotAnInteger(rule)),
            },
            Rule::array_type => {
                let vec_contents: Vec<Pair<'_, Rule>> = pair.clone().into_inner().collect();
                let ty = Box::new(Ty::from_pair(&vec_contents[0])?);
                let size = Constant::from_str(vec_contents[1].as_str())?;
                Ok(Ty::Array { ty, size })
            }
            Rule::identifier_type => {
                let mut iter = pair.clone().into_inner();
                let id = String::from(iter.next().unwrap().as_str());
                let nullable = if let Some(pair) = iter.next() {
                    match pair.as_rule() {
                        Rule::nullable => true,
                        e => { return Err(ParseError::UnexpectedToken(e)); }
                    }
                } else {
                    false
                };
                Ok(Ty::Ident{id, nullable})
            }
            Rule::string_type => {
                let mut size = None;
                let mut nullable = false;
                for inner_pair in pair.clone().into_inner() {
                    match inner_pair.as_rule() {
                        Rule::constant => {
                            size = Some(Constant::from_str(inner_pair.as_str())?);
                        }
                        Rule::nullable => {
                            nullable = true;
                        }
                        e => { return Err(ParseError::UnexpectedToken(e)); }
                    }
                }
                Ok(Ty::Str { size, nullable })
            }
            Rule::vector_type => {
                let mut iter = pair.clone().into_inner();
                let ty = Box::new(Ty::from_pair(&iter.next().unwrap())?);
                let mut size = None;
                let mut nullable = false;
                for inner_pair in iter {
                    match inner_pair.as_rule() {
                        Rule::constant => {
                            size = Some(Constant::from_str(inner_pair.as_str())?);
                        }
                        Rule::nullable => {
                            nullable = true;
                        }
                        e => { return Err(ParseError::UnexpectedToken(e)); }
                    }
                }
                Ok(Ty::Vector { ty, size, nullable })
            }
            _e => Err(ParseError::UnrecognizedType(
                pair.as_str().to_string(),
            )),
        }
    }
}

// TODO string should be an Ident type
#[derive(PartialEq, Eq, Serialize, Debug, Hash)]
pub struct StructField {
    pub attributes: Attrs,
    pub ty: Ty,
    pub ident: String,
}
impl StructField {
    pub fn from_pair(pair: Pair<'_, Rule>) -> Result<Self, ParseError> {
        let mut attributes = Attrs::default();
        let mut ty = None;
        let mut ident = String::default();
        for inner_pair in pair.into_inner() {
            match inner_pair.as_rule() {
                Rule::attributes => {
                    attributes = Attrs::from_pair(inner_pair)?;
                }
                Rule::ident => {
                    ident = String::from(inner_pair.as_str());
                }
                _ => {
                    ty = Some(Ty::from_pair(&inner_pair)?);
                }
            }
        }
        // TODO use the val field in structs
        Ok(StructField {
            attributes: attributes,
            ty: ty.unwrap(),
            ident: ident,
            //            val: String::from(fields[2].as_str()),
        })
    }
}

// TODO string should be an Ident type
#[derive(PartialEq, Eq, Serialize, Debug, Hash)]
pub struct UnionField {
    pub attributes: Attrs,
    pub ty: Ty,
    pub ident: String,
}
impl UnionField {
    pub fn from_pair(pair: Pair<'_, Rule>) -> Result<Self, ParseError> {
        let fields: Vec<Pair<'_, Rule>> = pair.into_inner().collect();
        let ty = &fields[1];
        Ok(UnionField {
            attributes: Attrs::from_pair(fields[0].clone())?,
            ty: Ty::from_pair(ty)?,
            ident: String::from(fields[2].as_str()),
        })
    }
}

// TODO string should be an Ident type
#[derive(PartialEq, Eq, Serialize, Debug, Hash)]
pub struct EnumVariant {
    pub attributes: Attrs,
    pub name: String,
    pub size: Constant,
}
impl EnumVariant {
    pub fn from_pair(pair: Pair<'_, Rule>) -> Result<Self, ParseError> {
        let fields: Vec<Pair<'_, Rule>> = pair.into_inner().collect();
        Ok(EnumVariant {
            attributes: Attrs::from_pair(fields[0].clone())?,
            name: String::from(fields[1].as_str()),
            size: Constant::from_str(fields[2].as_str())?,
        })
    }
}

#[derive(PartialEq, Eq, Serialize, Debug, Hash)]
pub struct Method {
    pub attributes: Attrs,
    pub name: String,
    pub in_params: Vec<(String, Ty)>,
    pub out_params: Vec<(String, Ty)>,
}

impl Method {
    pub fn from_pair(pair: Pair<'_, Rule>) -> Result<Self, ParseError> {
        let mut attributes = Attrs::default();
        let mut name = String::default();
        let mut in_params = Vec::new();
        let mut out_params = Vec::new();
        for inner_pair in pair.into_inner() {
            match inner_pair.as_rule() {
                Rule::attributes => {
                    attributes = Attrs::from_pair(inner_pair)?;
                }
                Rule::interface_parameters => {
                    let mut fields: Vec<Pair<'_, Rule>> = inner_pair.into_inner().collect();
                    name = String::from(fields[0].as_str());
                    // TODO cleaner way of getting in/out params
                    let inner_params = fields.remove(1);
                    for in_pair in inner_params.into_inner() {
                        match in_pair.as_rule() {
                            Rule::parameter => {
                                let mut param = in_pair.into_inner();
                                let ty = Ty::from_pair(&param.next().unwrap())?;
                                let name = String::from(param.next().unwrap().as_str());
                                in_params.push((name, ty));
                            },
                            e => return Err(ParseError::UnexpectedToken(e)),
                        }
                    }
                    let inner_params = fields.remove(1);
                    for in_pair in inner_params.into_inner() {
                        match in_pair.as_rule() {
                            Rule::parameter => {
                                let mut param = in_pair.into_inner();
                                let ty = Ty::from_pair(&param.next().unwrap())?;
                                let name = String::from(param.next().unwrap().as_str());
                                out_params.push((name, ty));
                            },
                            e => return Err(ParseError::UnexpectedToken(e)),
                        }
                    }
                }
                e => return Err(ParseError::UnexpectedToken(e)),
            }
        }
        Ok(Method {
            attributes,
            name,
            in_params,
            out_params,
        })
    }
}

#[derive(PartialEq, Eq, Serialize, Debug, Hash)]
pub enum Decl {
    Struct {
        attributes: Attrs,
        name: String,
        fields: Vec<StructField>,
    },
    Interface{
        attributes: Attrs,
        name: String,
        methods: Vec<Method>,
    },
    Alias(String, String),
    Constant {
        attributes: Attrs,
        name: String,
        ty: Ty,
        value: Constant,
    },
    Union {
        attributes: Attrs,
        name: String,
        fields: Vec<UnionField>,
    },
    Enum {
        attributes: Attrs,
        name: String,
        ty: Ty,
        variants: Vec<EnumVariant>,
    },
}

#[derive(PartialEq, Serialize, Debug)]
pub struct BanjoAst {
    pub primary_namespace: String,
    pub namespaces: HashMap<String, Vec<Decl>>,
}

impl BanjoAst {
    pub fn id_to_type(&self, fq_ident: &str) -> Ty {
        // check if FQ
        let v: Vec<&str> = fq_ident.rsplitn(2, '.').collect();
        let mut namespace = self.primary_namespace.as_str();
        let mut ident = fq_ident;
        if v.len() > 1 {
            namespace = v[1];
            ident = v[0];
        }

        match ident {
            "usize" => return Ty::USize,
            "bool" => return Ty::Bool,
            "int8" => return Ty::Int8,
            "int16" => return Ty::Int16,
            "int32" => return Ty::Int32,
            "int64" => return Ty::Int64,
            "uint8" => return Ty::UInt8,
            "uint16" => return Ty::UInt16,
            "uint32" => return Ty::UInt32,
            "uint64" => return Ty::UInt64,
            "float32" => return Ty::Float32,
            "float64" => return Ty::Float64,
            "voidptr" => return Ty::Voidptr,
            _ => {}
        };

        for decl in self.namespaces[namespace].iter() {
            match decl {
                Decl::Interface {name, ..} => {
                    if name == ident {
                        return Ty::Interface;
                    }
                }
                Decl::Struct {name, ..} => {
                    if name == ident {
                        return Ty::Struct;
                    }
                },
                Decl::Union {name, ..} => {
                    if name == ident {
                        return Ty::Union;
                    }
                },
                Decl::Enum {name, ..} => {
                    if name == ident {
                        return Ty::Enum;
                    }
                },
                Decl::Alias (to, from) => {
                    if to == ident {
                        return self.id_to_type(from);
                    }
                },
                Decl::Constant {name, ty, ..} => {
                    if name == ident {
                        return (*ty).clone();
                    }
                },
            }
        }
        panic!("Unidentified {}", fq_ident);
    }

    pub fn parse_decl(
        pair: Pair<'_, Rule>,
        _namespaces: &HashMap<String, Vec<Decl>>,
    ) -> Result<Decl, ParseError> {
        match pair.as_rule() {
            Rule::struct_declaration => {
                let mut attributes = Attrs::default();
                let mut name = String::default();
                let mut fields = Vec::default();
                for inner_pair in pair.into_inner() {
                    match inner_pair.as_rule() {
                        Rule::attributes => {
                            attributes = Attrs::from_pair(inner_pair)?;
                        }
                        Rule::ident => {
                            name = String::from(inner_pair.as_str());
                        }
                        Rule::struct_field => fields.push(StructField::from_pair(inner_pair)?),
                        e => return Err(ParseError::UnexpectedToken(e)),
                    }
                }
                Ok(Decl::Struct {
                    attributes,
                    name,
                    fields,
                })
            }
            Rule::enum_declaration => {
                let mut attributes = Attrs::default();
                let mut name = String::default();
                let mut variants = Vec::default();
                let mut ty = Ty::UInt16;
                for inner_pair in pair.into_inner() {
                    match inner_pair.as_rule() {
                        Rule::attributes => {
                            attributes = Attrs::from_pair(inner_pair)?;
                        }
                        Rule::ident => {
                            name = String::from(inner_pair.as_str());
                        }
                        Rule::integer_type => {
                            ty = Ty::from_pair(&inner_pair)?;
                        }
                        Rule::enum_field => variants.push(EnumVariant::from_pair(inner_pair)?),
                        e => return Err(ParseError::UnexpectedToken(e)),
                    }
                }
                Ok(Decl::Enum {
                    attributes,
                    name,
                    ty,
                    variants,
                })
            }
            Rule::union_declaration => {
                let mut attributes = Attrs::default();
                let mut name = String::default();
                let mut fields = Vec::default();
                for inner_pair in pair.into_inner() {
                    match inner_pair.as_rule() {
                        Rule::attributes => {
                            attributes = Attrs::from_pair(inner_pair)?;
                        }
                        Rule::ident => {
                            name = String::from(inner_pair.as_str());
                        }
                        Rule::union_field => fields.push(UnionField::from_pair(inner_pair)?),
                        e => return Err(ParseError::UnexpectedToken(e)),
                    }
                }
                Ok(Decl::Union {
                    attributes,
                    name,
                    fields,
                })
            }
            // TODO extend to be more expressive for banjo
            Rule::interface_declaration => {
                let mut attributes = Attrs::default();
                let mut name = String::default();
                let mut methods = Vec::default();
                for inner_pair in pair.into_inner() {
                    match inner_pair.as_rule() {
                        Rule::attributes => {
                            attributes = Attrs::from_pair(inner_pair)?;
                        }
                        Rule::ident => {
                            name = String::from(inner_pair.as_str());
                        }
                        Rule::interface_method => methods.push(Method::from_pair(inner_pair)?),
                        e => return Err(ParseError::UnexpectedToken(e)),
                    }
                }
                Ok(Decl::Interface {
                    attributes,
                    name,
                    methods,
                })
            }
            Rule::const_declaration => {
                let mut attributes = Attrs::default();
                let mut name = String::default();
                let mut ty = Ty::UInt32;
                let mut value = Constant::SizedRaw(0);
                for inner_pair in pair.into_inner() {
                    match inner_pair.as_rule() {
                        Rule::attributes => {
                            attributes = Attrs::from_pair(inner_pair)?;
                        }
                        Rule::ident => {
                            name = String::from(inner_pair.as_str());
                        }
                        Rule::primitive_type => {
                            ty = Ty::from_pair(&inner_pair)?;
                        }
                        Rule::constant => {
                            value = Constant::from_str(inner_pair.clone().into_span().as_str())?;
                        }
                        e =>  {
                            return Err(ParseError::UnexpectedToken(e))
                        }
                    }
                }
                Ok(Decl::Constant {
                    attributes,
                    name,
                    ty,
                    value,
                })
            }
            e => {
                Err(ParseError::UnexpectedToken(e))
            }
        }
    }

    fn type_to_decl(&self, ty: &Ty) -> Option<&Decl> {
	match ty {
	    Ty::Array{ref ty, ..} => {
		self.type_to_decl(ty)
	    }
	    Ty::Vector{ref ty, ..} => {
		self.type_to_decl(ty)
	    }
	    Ty::Ident{id, ..} => {
		// check if FQ
		let v: Vec<&str> = id.rsplitn(2, '.').collect();
		let (namespace, ident) =  if v.len() > 1 {
		    (v[1], v[0])
		} else {
		    (self.primary_namespace.as_str(), id.as_str())
		};
		for decl in self.namespaces[namespace].iter() {
		    match decl {
			Decl::Interface {name, ..} |
			Decl::Struct {name, ..} |
			Decl::Union {name, ..} |
			Decl::Enum {name, ..} |
			Decl::Constant {name, ..} => {
			    if name == ident {
				return Some(decl);
			    }
			},
			Decl::Alias (to, from) => {
			    if to == ident {
				return self.type_to_decl(&self.id_to_type(from));
			    }
			},
		    }
		}
		None
	    }
	    _ => None,
	}
    }

    // An edge from D1 to D2 means that a C needs to see the declaration
    // of D1 before the declaration of D2. For instance, given the banjo
    //     struct D2 { D1 d; };
    //     struct D1 { int32 x; };
    // D1 has an edge pointing to D2. Note that struct and union pointers,
    // unlike inline structs or unions, do not have dependency edges.
    fn decl_dependencies(&self, decl: &Decl) -> Result<HashSet<&Decl>, ParseError> {
	let mut edges = HashSet::new();

	let mut maybe_add_decl = |ty| {
	    if let Some(type_decl) = self.type_to_decl(ty) {
		edges.insert(type_decl);
	    }
	};

	match decl {
	    Decl::Interface { methods, ..} => {
		for method in methods {
		    for (_, ty) in method.in_params.iter() {
			maybe_add_decl(&ty);
		    }
		    for (_, ty) in method.out_params.iter() {
			maybe_add_decl(&ty);
		    }
		}
	    }
	    Decl::Struct { fields, ..} => {
		for field in fields {
		    maybe_add_decl(&field.ty);
		}
	    }
	    Decl::Union {fields, ..} => {
		for field in fields {
		    maybe_add_decl(&field.ty);
		}
	    }
	    Decl::Alias (_to, from) =>  {
		maybe_add_decl(&self.id_to_type(from));
	    }
	    // TODO(surajmalhtora): Implement constant.
	    Decl::Constant {..} => (),
	    // Enum cannot have dependencies.
	    Decl::Enum {..} => (),
	};

	Ok(edges)
    }

    // Validates that the declarations are cycle free.
    fn validate_declaration_deps(&self) -> Result<(), ParseError> {
	// The number of undelcared dependencies for each decl.
	let mut degrees: HashMap<&Decl, u32> = HashMap::new();
	// Records the decls that depend on each other.
	let mut inverse_dependencies: HashMap<&Decl, Vec<&Decl>> = HashMap::new();

	for decl in self.namespaces.iter().flat_map(|(_, decls)| decls.iter()) {
	    degrees.insert(&decl, 0);
	}

	for decl in self.namespaces.iter().flat_map(|(_, decls)| decls.iter()) {
	    let deps = self.decl_dependencies(&decl)?;
	    for dep in deps.iter().filter(|&dep| dep != &decl) {
		let entry = degrees.get_mut(&decl).unwrap();
		*entry += 1;
		let entry = inverse_dependencies.entry(&dep).or_insert(Vec::new());
		entry.push(&decl);
	    }
	}
	// Remove mutability.
	let inverse_dependencies = inverse_dependencies;

	// Start with all decls that have no incoming edges.
	let mut decls_without_deps = degrees.iter()
	                                    .filter(|(_, &degrees)| degrees == 0)
					    .map(|(&decl, _)| decl)
					    .collect::<VecDeque<_>>();

	let mut decl_order = Vec::new();
	// Pull one out of the queue.
	while let Some(decl) = decls_without_deps.pop_front() {
	    assert_eq!(degrees.get(decl), Some(&0));
	    decl_order.push(decl);

	    // Decrement the incoming degree of all other decls it points to.
	    if let Some(inverse_deps) = inverse_dependencies.get(decl) {
		for inverse_dep in inverse_deps {
		    let degree = degrees.get_mut(inverse_dep).unwrap();
		    assert_ne!(*degree, 0);
		    *degree -= 1;
		    if *degree == 0 {
			decls_without_deps.push_back(inverse_dep);
		    }
		}
	    }
	}

	if decl_order.len() != degrees.len() {
	    // We didn't visit all the edges! There was a cycle.
	    return Err(ParseError::InvalidDeps(String::from("There is a cycle in the declarations")));
	}

	Ok(())
    }

    pub fn parse(pair_vec: Vec<Pairs<'_, Rule>>) -> Result<Self, ParseError> {
        let mut primary_namespace = None;
        let mut namespaces = HashMap::default();

        for pairs in pair_vec {
            for pair in pairs {
                let mut current_namespace = String::default();
                let mut namespace = Vec::default();
                for inner_pair in pair.into_inner() {
                    match inner_pair.as_rule() {
                        Rule::library_header => {
                            for token in inner_pair.into_inner() {
                                if Rule::compound_ident == token.as_rule() {
                                    current_namespace = String::from(token.as_str());
                                    if let Some(primary_namespace) = primary_namespace {
                                        if primary_namespace == current_namespace {
                                            return Err(ParseError::AlreadyPrimaryNamespace);
                                        }
                                    }
                                    primary_namespace = Some(String::from(token.as_str()));
                                }
                            }
                        }
                        Rule::using_decl => {
                            let contents: Vec<&str> = inner_pair
                                .clone()
                                .into_inner()
                                .map(|p| p.as_str())
                                .collect();
                            namespace.push(Decl::Alias(
                                contents[0].to_string(),
                                contents[1].to_string(),
                            ));
                        }
                        Rule::using => {
                            for (cnt, pair) in inner_pair.into_inner().enumerate() {
                                if cnt == 0 {
                                    if !namespaces.contains_key(pair.as_str()) {
                                        return Err(ParseError::UnImported(format!(
                                            "{}",
                                            pair.as_str()
                                        )));
                                    }
                                } else {
                                    return Err(ParseError::NotYetSupported(String::from(
                                        "'as' in library imports",
                                    )));
                                }
                            }
                        }
                        Rule::struct_declaration
                        | Rule::union_declaration
                        | Rule::enum_declaration
                        | Rule::interface_declaration
                        | Rule::const_declaration => {
                            let decl = Self::parse_decl(inner_pair, &namespaces)?;
                            namespace.push(decl)
                        }
                        Rule::EOI => (),
                        e => return Err(ParseError::UnexpectedToken(e)),
                    };
                }
                namespaces.insert(current_namespace, namespace);
            }
        }

        let ast = BanjoAst {
            primary_namespace: primary_namespace.unwrap(),
            namespaces,
        };
	ast.validate_declaration_deps()?;

	Ok(ast)
    }
}
