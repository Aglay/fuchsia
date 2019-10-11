// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    blackout_target::{
        static_tree::{DirectoryEntry, EntryDistribution},
        CommonCommand, CommonOpts,
    },
    failure::{Error, ResultExt},
    fs_management::{Filesystem, Minfs},
    rand::{rngs::StdRng, Rng, SeedableRng},
    structopt::StructOpt,
};

#[derive(Debug, StructOpt)]
#[structopt(rename_all = "kebab-case")]
struct Opts {
    #[structopt(flatten)]
    common: CommonOpts,
    /// A particular step of the test to perform.
    #[structopt(subcommand)]
    commands: CommonCommand,
}

fn setup(minfs: Filesystem<Minfs>) -> Result<(), Error> {
    println!("formatting block device with minfs");
    minfs.format().context("failed to format minfs")?;

    Ok(())
}

fn test(mut minfs: Filesystem<Minfs>, seed: u64) -> Result<(), Error> {
    let root = format!("/test-fs-root-{}", seed);

    println!("mounting minfs into default namespace at {}", root);
    minfs.mount(&root).context("failed to mount minfs")?;

    println!("generating load");
    let mut rng = StdRng::seed_from_u64(seed);
    loop {
        println!("generating tree");
        let dist = EntryDistribution::new(6);
        let tree: DirectoryEntry = rng.sample(&dist);
        println!("generated tree: {:?}", tree);
        println!("writing tree");
        tree.write_tree(&root).context("failed to write directory tree")?;
        // it's entirely possible that we generate a tree small enough that we manage to write the
        // whole thing before the test is stopped. if that happens, write another tree!
    }
}

fn verify(minfs: Filesystem<Minfs>) -> Result<(), Error> {
    println!("verifying disk with fsck");
    minfs.fsck().context("failed to run fsck")?;

    println!("verification successful");
    Ok(())
}

fn main() -> Result<(), Error> {
    let opts = Opts::from_args();

    println!("minfs block device: {}", opts.common.block_device);
    let minfs = Minfs::new(&opts.common.block_device)?;

    match opts.commands {
        CommonCommand::Setup => setup(minfs),
        CommonCommand::Test => test(minfs, opts.common.seed),
        CommonCommand::Verify => verify(minfs),
    }
}
