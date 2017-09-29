// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Library and runtime for fidl bindings.

#![deny(missing_docs)]

extern crate fuchsia_zircon as zircon;
extern crate byteorder;
extern crate futures;
#[macro_use]
extern crate tokio_core;
extern crate tokio_fuchsia;

#[macro_use]
mod encoding;
mod message;
mod error;
mod server;
mod interface;
mod cookiemap;
mod client;
mod endpoints;

pub use encoding::{Encodable, Decodable, EncodablePtr, DecodablePtr, EncodableType};
pub use encoding::{CodableUnion, EncodableNullable, DecodableNullable};
pub use encoding::{encode_handle, decode_handle};
pub use message::{EncodeBuf, DecodeBuf, MsgType};
pub use error::{Error, Result};
pub use server::{Stub, Server};
pub use interface::InterfacePtr;
pub use client::{Client, FidlService};
pub use endpoints::{ClientEnd, ServerEnd};

/// A specialized `Box<Future<...>>` type for FIDL.
/// This is mostly as a convenience to avoid writing
/// `Box<Future<Item = I, Error = fidl::Error> + Send>`.
pub type BoxFuture<Item> = Box<futures::Future<Item = Item, Error = Error> + Send>;