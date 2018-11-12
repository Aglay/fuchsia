// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use structopt::StructOpt;

#[derive(StructOpt, Debug)]
pub enum Opt {
    #[structopt(name = "if")]
    /// commands for network interfaces
    If(IfCmd),

    #[structopt(name = "fwd")]
    /// commands for forwarding tables
    Fwd(FwdCmd),
}

#[derive(StructOpt, Clone, Debug)]
pub enum IfCmd {
    #[structopt(name = "list")]
    /// lists network interfaces
    List,
    #[structopt(name = "add")]
    /// adds a network interface by path
    Add {
        #[structopt(raw(required = "true"))]
        // The path must yield a handle to a zircon.ethernet.Device interface.
        // Currently this means paths under /dev/class/ethernet.
        /// path to the device to add
        path: String,
    },
    #[structopt(name = "del")]
    /// removes a network interface
    Del {
        #[structopt(raw(required = "true"))]
        /// id of the network interface to remove
        id: u64,
    },
    #[structopt(name = "get")]
    /// queries a network interface
    Get {
        #[structopt(raw(required = "true"))]
        /// id of the network interface to query
        id: u64,
    },
    #[structopt(name = "enable")]
    /// enables a network interface
    Enable {
        #[structopt(raw(required = "true"))]
        /// id of the network interface to enable
        id: u64,
    },
    #[structopt(name = "disable")]
    /// disables a network interface
    Disable {
        #[structopt(raw(required = "true"))]
        /// id of the network interface to disable
        id: u64,
    },
    #[structopt(name = "addr")]
    /// commands for updating network interface addresses
    Addr(AddrCmd),
}

#[derive(StructOpt, Clone, Debug)]
pub enum AddrCmd {
    #[structopt(name = "add")]
    /// adds an address to the network interface
    Add {
        #[structopt(raw(required = "true"))]
        /// id of the network interface
        id: u64,
        #[structopt(raw(required = "true"))]
        addr: String,
        #[structopt(raw(required = "true"))]
        prefix: u8,
    },
    #[structopt(name = "del")]
    /// deletes an address from the network interface
    Del {
        #[structopt(raw(required = "true"))]
        /// id of the network interface
        id: u64,
        #[structopt(raw(required = "true"))]
        addr: String,
    },
}

#[derive(StructOpt, Copy, Clone, Debug)]
pub enum FwdCmd {
    #[structopt(name = "list")]
    /// lists forwarding table entries
    List,
    // TODO: add/del
}
