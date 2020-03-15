// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A Fuchsia Driver Bind Program compiler

use anyhow::{Context, Error};
use bind_debugger::instruction::{Condition, Instruction, InstructionDebug};
use bind_debugger::{compiler, offline_debugger};
use std::fmt::Write;
use std::fs::File;
use std::io::{self, BufRead, Write as IoWrite};
use std::path::PathBuf;
use structopt::StructOpt;

const AUTOBIND_PROPERTY: u32 = 0x0002;

#[derive(StructOpt, Debug)]
struct Opt {
    /// Output file. The compiler emits a C header file.
    #[structopt(short = "o", long = "output", parse(from_os_str))]
    output: Option<PathBuf>,

    /// The bind program input file. This should be in the format described in
    /// //tools/bindc/README.md.
    #[structopt(parse(from_os_str))]
    input: PathBuf,

    /// The bind library input files. These may be included by the bind program. They should be in
    /// the format described in //tools/bindc/README.md.
    #[structopt(short = "i", long = "include", parse(from_os_str))]
    include: Vec<PathBuf>,

    /// Specifiy the bind library input files as a file. The file must contain a list of filenames
    /// that are bind library input files that may be included by the bind program. Those files
    /// should be in the format described in //tools/bindc/README.md.
    #[structopt(short = "f", long = "include-file", parse(from_os_str))]
    include_file: Option<PathBuf>,

    /// Specify a path for the compiler to generate a depfile. A depfile contain, in Makefile
    /// format, the files that this invocation of the compiler depends on including all bind
    /// libraries and the bind program input itself. An output file must be provided to generate a
    /// depfile.
    #[structopt(long = "depfile", parse(from_os_str))]
    depfile: Option<PathBuf>,

    /// A file containing the properties of a specific device, as a list of key-value pairs.
    /// This will be used as the input to the bind program debugger.
    /// In debug mode no compiler output is produced, so --output should not be specified.
    #[structopt(short = "d", long = "debug", parse(from_os_str))]
    device_file: Option<PathBuf>,

    // TODO(43400): Eventually this option should be removed when we can define this configuration
    // in the driver's component manifest.
    /// Disable automatically binding the driver so that the driver must be bound on a user's
    /// request.
    #[structopt(short = "a", long = "disable-autobind")]
    disable_autobind: bool,
}

fn write_depfile(output: &PathBuf, input: &PathBuf, includes: &[PathBuf]) -> Result<String, Error> {
    fn path_to_str(path: &PathBuf) -> Result<&str, Error> {
        path.as_os_str().to_str().context("failed to convert path to string")
    };

    let output_str = path_to_str(output)?;
    let input_str = path_to_str(input)?;
    let mut deps = includes.iter().map(|s| path_to_str(s)).collect::<Result<Vec<&str>, Error>>()?;
    deps.push(input_str);

    let mut out = String::new();
    writeln!(&mut out, "{}: {}", output_str, deps.join(" "))?;
    Ok(out)
}

fn write_bind_template(
    mut instructions: Vec<InstructionDebug>,
    disable_autobind: bool,
) -> Option<String> {
    if disable_autobind {
        instructions.insert(
            0,
            InstructionDebug::new(Instruction::Abort(Condition::NotEqual(AUTOBIND_PROPERTY, 0))),
        );
    }
    let bind_count = instructions.len();
    let binding = instructions
        .into_iter()
        .map(|instr| instr.encode())
        .map(|(word0, word1, word2)| format!("{{{:#x},{:#x},{:#x}}},", word0, word1, word2))
        .collect::<String>();
    let mut output = String::new();
    output
        .write_fmt(format_args!(
            include_str!("templates/bind.h.template"),
            bind_count = bind_count,
            binding = binding,
        ))
        .ok()?;
    Some(output)
}

fn main() {
    let opt = Opt::from_iter(std::env::args());

    if opt.output.is_some() && opt.device_file.is_some() {
        eprintln!("Error: options --output and --debug are mutually exclusive.");
        std::process::exit(1);
    }

    let mut includes = opt.include;

    if let Some(include_file) = opt.include_file {
        let file = File::open(include_file).unwrap();
        let reader = io::BufReader::new(file);
        let filenames = reader.lines().map(|line| {
            if line.is_err() {
                eprintln!("Failed to read include file");
                std::process::exit(1);
            }
            PathBuf::from(line.unwrap())
        });
        includes.extend(filenames);
    }

    if let Some(device_file) = opt.device_file {
        if let Err(err) = offline_debugger::debug(opt.input, &includes, device_file) {
            eprintln!("Debugger failed with error:");
            eprintln!("{}", err);
            std::process::exit(1);
        }
        return;
    }

    let mut output: Box<dyn io::Write> = if let Some(output) = opt.output {
        // If there's an output filename then we can generate a depfile too.
        if let Some(filename) = opt.depfile {
            let mut file = File::create(filename).unwrap();
            let depfile_string = write_depfile(&output, &opt.input, &includes);
            if depfile_string.is_err() {
                eprintln!("Failed to create depfile");
                std::process::exit(1);
            }
            let r = file.write(depfile_string.unwrap().as_bytes());
            if r.is_err() {
                eprintln!("Failed to write to depfile");
                std::process::exit(1);
            }
        }

        Box::new(File::create(output).unwrap())
    } else {
        Box::new(io::stdout())
    };

    match compiler::compile(opt.input, &includes) {
        Ok(instructions) => match write_bind_template(instructions, opt.disable_autobind) {
            Some(out_string) => {
                let r = output.write(out_string.as_bytes());
                if r.is_err() {
                    eprintln!("Failed to write to output");
                    std::process::exit(1);
                }
            }
            None => {
                eprintln!("Failed to format output");
                std::process::exit(1);
            }
        },
        Err(err) => {
            eprintln!("{}", err);
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_instructions() {
        let out_string = write_bind_template(vec![], false).unwrap();
        assert!(out_string.contains("ZIRCON_DRIVER_BEGIN(Driver, Ops, VendorName, Version, 0)"));
    }

    #[test]
    fn one_instruction() {
        let instructions = vec![InstructionDebug::new(Instruction::Match(Condition::Always))];
        let out_string = write_bind_template(instructions, false).unwrap();
        assert!(out_string.contains("ZIRCON_DRIVER_BEGIN(Driver, Ops, VendorName, Version, 1)"));
        assert!(out_string.contains("{0x1000000,0x0,0x0}"));
    }

    #[test]
    fn disable_autobind() {
        let instructions = vec![InstructionDebug::new(Instruction::Match(Condition::Always))];
        let out_string = write_bind_template(instructions, true).unwrap();
        assert!(out_string.contains("ZIRCON_DRIVER_BEGIN(Driver, Ops, VendorName, Version, 2)"));
        assert!(out_string.contains("{0x20000002,0x0,0x0}"));
    }

    #[test]
    fn depfile_no_includes() {
        let output = PathBuf::from("/a/output");
        let input = PathBuf::from("/a/input");
        assert_eq!(
            write_depfile(&output, &input, &[]).unwrap(),
            "/a/output: /a/input\n".to_string()
        );
    }

    #[test]
    fn depfile_with_includes() {
        let output = PathBuf::from("/a/output");
        let input = PathBuf::from("/a/input");
        let includes = vec![PathBuf::from("/a/include"), PathBuf::from("/b/include")];
        let result = write_depfile(&output, &input, &includes).unwrap();
        assert!(result.starts_with("/a/output:"));
        assert!(result.contains("/a/input"));
        assert!(result.contains("/a/include"));
        assert!(result.contains("/b/include"));
    }
}
