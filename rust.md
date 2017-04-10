Rust
====

Note: this is very much a work in progress, stay tuned for updates!


## Targets

There are two gn targets for building Rust:
- [`rust_library`][target-library] defines a library which can be used by other
targets;
- [`rust_binary`][target-binary] defines an executable.

Note that both targets can be used with the Fuchsia toolchain and with the host
toolchain.

These GN targets should be complemented by a
[`Cargo.toml` manifest file][manifest] very similar to that of a regular Rust
crate except for how dependencies are handled.

### Dependencies

There are two types of dependencies:
 - in-tree dependencies (e.g. FIDL, other rust_library targets);
 - third-party dependencies.

Third-party dependencies should be added to the manifest file just like in the
normal Rust development workflow - see the next section for how third-party
dependencies are inserted into the build.
In-tree dependencies should be added to the build file as target dependencies.

Here's an example of a library depending on the third-party crate `bitflags`
and on a FIDL library at `//apps/framework/services`:
```
BUILD.gn
--------
rust_library("my-library") {
  deps = [
    "//apps/framework/services:services_rust_library",
  ]
}

Cargo.toml
----------
[package]
name = "my-library"
version = "0.1.0"

[dependencies]
bitflags = "0.7.0"
```


## Managing third-party dependencies

Third-party crates are stored in [`//third-party/rust-crates/vendor`][3p-crates]
which we use as a [directory source][source-replacement] automatically set up
by the build system.

To be able to run the update script, you first need to build the
`cargo-vendor` utility:
```
scripts/build_cargo_vendor.sh
```

To update these crates, run the following commands:
```
scripts/update_rust_packages.py --cargo-vendor third_party/rust-crates/manual/cargo-vendor/target/debug/cargo-vendor
build/rust/fetch-vendor-license.py --directory third_party/rust-crates/vendor
```
The configurations used as a reference to generate the set of required crates
are listed in the [`update_rust_packages.py`][update-script] script.

### Adding a new third-party dependency

If a crate is not avalable in the vendor directory, it needs to be added with the following steps.

1. Create your crate's manifest file and reference the crates you need;
1. Add your crate root to `CONFIGS` in the [update script][update-script];
1. Run the commands listed above.


[target-library]: https://fuchsia.googlesource.com/build/+/master/rust/rust_library.gni "Rust library"
[target-binary]: https://fuchsia.googlesource.com/build/+/master/rust/rust_binary.gni "Rust binary"
[manifest]: http://doc.crates.io/manifest.html "Manifest file"
[3p-crates]: https://fuchsia.googlesource.com/third_party/rust-crates/+/master/vendor "Third-party crates"
[source-replacement]: http://doc.crates.io/source-replacement.html "Source replacement"
[update-script]: https://fuchsia.googlesource.com/scripts/+/master/update_rust_crates.py "Update script"
