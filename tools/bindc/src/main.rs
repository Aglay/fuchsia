// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A Fuchsia Driver Bind Program compiler

use anyhow::{Context, Error};
use bind_debugger::instruction::{Condition, Instruction, InstructionDebug};
use bind_debugger::{compiler, offline_debugger};
use std::fmt::Write;
use std::fs::File;
use std::io::prelude::*;
use std::io::{self, BufRead, Write as IoWrite};
use std::path::PathBuf;
use structopt::StructOpt;

const AUTOBIND_PROPERTY: u32 = 0x0002;

#[derive(StructOpt, Debug)]
struct SharedOptions {
    /// The bind library input files. These may be included by the bind program. They should be in
    /// the format described in //tools/bindc/README.md.
    #[structopt(short = "i", long = "include", parse(from_os_str))]
    include: Vec<PathBuf>,

    /// Specifiy the bind library input files as a file. The file must contain a list of filenames
    /// that are bind library input files that may be included by the bind program. Those files
    /// should be in the format described in //tools/bindc/README.md.
    #[structopt(short = "f", long = "include-file", parse(from_os_str))]
    include_file: Option<PathBuf>,

    /// The bind program input file. This should be in the format described in
    /// //tools/bindc/README.md.
    #[structopt(parse(from_os_str))]
    input: PathBuf,
}

#[derive(StructOpt, Debug)]
enum Command {
    #[structopt(name = "compile")]
    Compile {
        #[structopt(flatten)]
        options: SharedOptions,

        /// Output file. The compiler emits a C header file.
        #[structopt(short = "o", long = "output", parse(from_os_str))]
        output: Option<PathBuf>,

        /// Specify a path for the compiler to generate a depfile. A depfile contain, in Makefile
        /// format, the files that this invocation of the compiler depends on including all bind
        /// libraries and the bind program input itself. An output file must be provided to generate a
        /// depfile.
        #[structopt(short = "d", long = "depfile", parse(from_os_str))]
        depfile: Option<PathBuf>,

        // TODO(43400): Eventually this option should be removed when we can define this configuration
        // in the driver's component manifest.
        /// Disable automatically binding the driver so that the driver must be bound on a user's
        /// request.
        #[structopt(short = "a", long = "disable-autobind")]
        disable_autobind: bool,
    },
    #[structopt(name = "debug")]
    Debug {
        #[structopt(flatten)]
        options: SharedOptions,

        /// A file containing the properties of a specific device, as a list of key-value pairs.
        /// This will be used as the input to the bind program debugger.
        #[structopt(short = "d", long = "debug", parse(from_os_str))]
        device_file: PathBuf,
    },
}

fn main() {
    let command = Command::from_iter(std::env::args());
    if let Err(err) = handle_command(command) {
        eprintln!("{}", err);
        std::process::exit(1);
    }
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
) -> Result<String, Error> {
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
        .context("Failed to format output")?;
    Ok(output)
}

fn read_file(path: &PathBuf) -> Result<String, Error> {
    let mut file = File::open(path)?;
    let mut buf = String::new();
    file.read_to_string(&mut buf)?;
    Ok(buf)
}

fn handle_command(command: Command) -> Result<(), Error> {
    match command {
        Command::Debug { options, device_file } => {
            let includes = handle_includes(options.include, options.include_file)?;
            let includes = includes.iter().map(read_file).collect::<Result<Vec<String>, _>>()?;
            let program = read_file(&options.input)?;
            let (instructions, symbol_table) = compiler::compile_to_symbolic(&program, &includes)?;

            let device = read_file(&device_file)?;
            offline_debugger::debug_from_str(&instructions, &symbol_table, &device)?;
            Ok(())
        }
        Command::Compile { options, output, depfile, disable_autobind } => {
            let includes = handle_includes(options.include, options.include_file)?;
            handle_compile(options.input, includes, disable_autobind, output, depfile)
        }
    }
}

fn handle_includes(
    mut includes: Vec<PathBuf>,
    include_file: Option<PathBuf>,
) -> Result<Vec<PathBuf>, Error> {
    if let Some(include_file) = include_file {
        let file = File::open(include_file).context("Failed to open include file")?;
        let reader = io::BufReader::new(file);
        let mut filenames = reader
            .lines()
            .map(|line| line.map(PathBuf::from))
            .map(|line| line.context("Failed to read include file"))
            .collect::<Result<Vec<_>, Error>>()?;
        includes.append(&mut filenames);
    }
    Ok(includes)
}

fn handle_compile(
    input: PathBuf,
    includes: Vec<PathBuf>,
    disable_autobind: bool,
    output: Option<PathBuf>,
    depfile: Option<PathBuf>,
) -> Result<(), Error> {
    let mut output_writer: Box<dyn io::Write> = if let Some(output) = output {
        // If there's an output filename then we can generate a depfile too.
        if let Some(filename) = depfile {
            let mut file = File::create(filename).context("Failed to open depfile")?;
            let depfile_string =
                write_depfile(&output, &input, &includes).context("Failed to create depfile")?;
            file.write(depfile_string.as_bytes()).context("Failed to write to depfile")?;
        }
        Box::new(File::create(output).context("Failed to create output file")?)
    } else {
        Box::new(io::stdout())
    };

    let program = read_file(&input)?;
    let includes = includes.iter().map(read_file).collect::<Result<Vec<String>, _>>()?;
    let instructions = compiler::compile(&program, &includes)?;
    let output_string = write_bind_template(instructions, disable_autobind)?;

    output_writer.write(output_string.as_bytes()).context("Failed to write to output")?;
    Ok(())
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
