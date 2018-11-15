// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[deny(warnings)]
use failure::{err_msg, Error};
use fuchsia_bluetooth::hci;
use std::fs::{File, OpenOptions};
use std::path::Path;

fn usage(appname: &str) {
    eprintln!("usage: {} add", appname);
    eprintln!("       {} rm DEVICE", appname);
    eprintln!("");
    eprintln!("Add a new fake-hci device or remove DEVICE");
    eprintln!("");
    eprintln!("examples: {} add           - Adds a new randomly named device", appname);
    eprintln!("          {} rm bt-hci-129 - Removes the device /dev/test/test/bt-hci-129", appname);
}

fn open_rdwr<P: AsRef<Path>>(path: P) -> Result<File, Error> {
    OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .map_err(|e| e.into())
}

fn rm_device(dev_name: &str) -> Result<(), Error> {
    let path = Path::new(hci::DEV_TEST).join(dev_name);
    let path_str = path.to_str().unwrap_or("<invalid path>");
    let dev = open_rdwr(path.clone())
        .map_err(|_| err_msg(format!("Cannot open fake hci device: {}", path_str)))?;
    hci::destroy_device(&dev).map(|_| println!("Device {} successfully removed", path_str))
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    let appname = &args[0];
    if args.len() < 2 {
        usage(appname);
        return;
    }
    // TODO(bwb) better arg parsing
    let command = &args[1];

    match command.as_str() {
        "add" => {
            match hci::create_and_bind_device() {
                Ok((_, id)) => println!("fake device added: {}", id),
                Err(e) => eprintln!("{}", e),
            };
        }
        "rm" => {
            if args.len() < 3 {
                usage(appname);
            } else {
                let device = args[2].as_str();
                if let Err(err) = rm_device(device) {
                    eprintln!("Could not remove device {}. Error: {}", device, err);
                }
            }
        }
        _ => usage(appname),
    };
}
