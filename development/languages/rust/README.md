# Rust

## Targets

There are two new GN targets which should be used for new projects:
- [`rustc_library`][target-library-rustc] defines a library which can be used
  by other targets.
- [`rustc_binary`][target-binary-rustc] defines an executable.

These GN targets do not require a `Cargo.toml` file, as they are not built with
Cargo during a normal build of Fuchsia. However, a `Cargo.toml` file can be
created for these targets by running `fx gen-cargo --target path/to/target:label`.
In order to generate a Cargo.toml, there must have first been a successful build
of Fuchsia which included the target. Generating `Cargo.toml` files is useful
for integration with IDEs such as Intellij and VSCode, and for using the
[`fargo`][fargo] tool to build and test targets without going through a full
GN build and run cycle each time.

## The Old Build System

### Deprecated: Targets

There are two *deprecated* gn targets for building Rust:
- [`rust_library`][target-library] defines a library which can be used by other
targets;
- [`rust_binary`][target-binary] defines an executable.

Note that both targets can be used with the Fuchsia toolchain and with the host
toolchain.

These GN targets must be complemented by a
[`Cargo.toml` manifest file][manifest] just like that of a regular Rust
crate.

### Migration From Old to New

Owners of existing targets should work to move to the `rustc_library` and
`rustc_binary` rules. An example of such a migration can be seen
[here][wlantool-migration]. Third-party crates formerly listed in Cargo.toml
can be included as dependencies named
`//third_party/rust-crates/rustc_deps:<cratename>`.
First-party libraries can be depended upon directly via their `rustc_library`
GN target. FIDL crates can be included by depending on the `fidl` target
with a `-rustc` suffix appended, like
`//garnet/lib/wlan/fidl:fidl-rustc` or `//garnet/lib/wlan/fidl:service-rustc`.

### Deprecated: Workspace

`garnet/Cargo.toml` establishes a workspace for all the crates in the garnet
subtree of Fuchsia. All crates in garnet that are part of the build must be listed
in the members array of the workspace. All crates in Garnet that appear in the
dependencies section of any crate in Garnet should have a matching patch
statement in the workspace file. Refer to the workspace file for examples.

When adding a new crate to Garnet it is important to add it to both sections
of the workspace file.

### Deprecated: FIDL Facade Crates

A Cargo workspace requires that all crates in the workspace live in the same file
system subtree as the workspace file. Fuchsia build system rules do not allow generated
files to live in the source tree. To resolve this conflict there are facade crates
in the garnet tree that locate the generated code at compile time and include it with
`include!()`. See `garnet/public/rust/fidl_crates/garnet_examples_fidl_services` for
an example.

### Deprecated: Non-Rust Dependencies

If a Garnet Rust crate depends on something that cannot be expressed in Cargo it
must specify that dependency in BUILD.gn. The most common case of this is when
a Rust crate depends on one of the FIDL facade crates.

This is true for transitive dependencies, so if crate A depends on crate B which
depends on a non-Rust dependency, there must be a gn dependency between A and B as
well as B and C.

Here's an example of a library depending on a FIDL library
at `//garnet/examples/fidl/services`:

```
BUILD.gn
--------
import("//build/rust/rust_library.gni")

rust_library("garnet_examples_fidl_services") {
  deps = [
    "//garnet/examples/fidl/services:services_rust",
  ]
}

Cargo.toml
----------
[package]
name = "garnet_examples_fidl_services"
version = "0.1.0"
license = "BSD-3-Clause"
authors = ["Rob Tsuk <robtsuk@google.com>"]
description = "Generated interface"
repository = "https://fuchsia.googlesource.com/garnet/"

[dependencies]
fidl = "0.1.0"
fuchsia-zircon = "0.3"
futures = "0.1.15"
tokio-core = "0.1"
tokio-fuchsia = "0.1.0"
```

### Deprecated: Testing

Both `rust_library` and `rust_binary` have a `with_tests` attribute which, if
set to _true_, will trigger unit tests associated with the target to be built
alongside the target and packaged in a test executable.

Integration tests are currently not supported.

## Building With a Custom Toolchain

If you want to test out Fuchsia with your own custom-built versions of rustc or cargo,
you can set the `rustc_prefix` argument to `fx set`, like this:

```
fx set x64 --release --args "rustc_prefix=\"/path/to/bin/dir\""
```

## Going further

- [Managing third-party dependencies](third_party.md)
- [Unsafe code](unsafe.md)


[target-library-rustc]: https://fuchsia.googlesource.com/build/+/master/rust/rustc_library.gni "Rust library"
[target-binary-rustc]: https://fuchsia.googlesource.com/build/+/master/rust/rustc_binary.gni "Rust binary"
[target-library]: https://fuchsia.googlesource.com/build/+/master/rust/rust_library.gni "Rust library"
[target-binary]: https://fuchsia.googlesource.com/build/+/master/rust/rust_binary.gni "Rust binary"
[manifest]: http://doc.crates.io/manifest.html "Manifest file"
[build-integration]: https://github.com/rust-lang/rust-roadmap/issues/12 "Build integration"
[cargo]: https://github.com/rust-lang/cargo "Cargo"
[cargo-vendor]: https://github.com/alexcrichton/cargo-vendor "cargo-vendor"
[fargo]: https://fuchsia.googlesource.com/fargo
[fidl]: https://fuchsia.googlesource.com/garnet/+/master/public/lib/fidl/ "FIDL"
[wlantool-migration]: https://fuchsia-review.googlesource.com/c/garnet/+/149537/7/bin/wlantool/BUILD.gn
