# GN Build Arguments

## All builds

### active_partition

**Current value (from the default):** `""`

From //build/images/args.gni:89

### add_qemu_to_build_archives
Whether to include images necessary to run Fuchsia in QEMU in build
archives.

**Current value (from the default):** `false`

From //build/images/args.gni:96

### additional_bootserver_arguments
Additional bootserver args to add to pave.sh. New uses of this should be
added with caution, and ideally discussion. The present use case is to
enable throttling of netboot when specific network adapters are combined
with specific boards, due to driver and hardware challenges.

**Current value (from the default):** `""`

From //build/images/args.gni:102

### all_font_file_paths
List of file paths to every font asset. Populated in fonts.gni.

**Current value (from the default):** `[]`

From //src/fonts/build/font_args.gni:35

### always_zedboot
Build boot images that prefer Zedboot over local boot (only for EFI).

**Current value (from the default):** `false`

From //build/images/args.gni:114

### asan_default_options
Default [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
options (before the `ASAN_OPTIONS` environment variable is read at
runtime).  This can be set as a build argument to affect most "asan"
variants in `known_variants` (which see), or overridden in
toolchain_args in one of those variants.  Note that setting this
nonempty may conflict with programs that define their own
`__asan_default_options` C function.

**Current value (from the default):** `""`

From //build/config/sanitizers/BUILD.gn:16

### audio_core_trace_enabled
Set to |true| to enable collecting execution traces of audio_core, or |false| to remove all
tracing overhead.

**Current value (from the default):** `true`

From //src/media/audio/audio_core/BUILD.gn:13

### auto_login_to_guest
Whether basemgr should automatically login as a persistent guest user.

**Current value (from the default):** `false`

From //src/modular/bin/basemgr/BUILD.gn:14

### auto_update_packages
Whether the component loader should automatically update packages.

**Current value (from the default):** `true`

From //src/sys/sysmgr/BUILD.gn:10

### avb_algorithm
AVB algorithm type.Supported options:
  SHA256_RSA2048
  SHA256_RSA4096
  SHA256_RSA8192
  SHA512_RSA2048
  SHA512_RSA4096
  SHA512_RSA8192

**Current value (from the default):** `"SHA512_RSA4096"`

From //build/images/vbmeta.gni:29

### avb_atx_metadata
AVB metadata which will be used to validate public key

**Current value (from the default):** `""`

From //build/images/vbmeta.gni:20

### avb_key
a key which will be used to sign VBMETA and images for AVB

**Current value (from the default):** `""`

From //build/images/vbmeta.gni:17

### base_cache_packages_allow_testonly
Whether to allow testonly=true targets in base/cache pacakges. Default to
true to allow testonly=true targets. It is preferrable to set to false for
production builds to avoid accidental inclusion of testing targets.

**Current value (from the default):** `true`

From //BUILD.gn:76

### base_package_labels
If you add package labels to this variable, the packages will be included in
the 'base' package set, which represents the set of packages that are part
of an OTA. These pacakages are updated as an atomic unit during an OTA
process and are immutable and are a superset of the TCB (Trusted Computing
Base) for a product. These packages are never evicted by the system.

**Current value for `target_cpu = "arm64"`:** `["//build/info:build-info", "//garnet/bin/http", "//src/connectivity/network/http_client", "//garnet/bin/log_listener:log_listener", "//garnet/bin/log_listener:log_listener_shell", "//garnet/bin/network_time_service", "//garnet/bin/scpi", "//garnet/bin/setui:setui_service", "//garnet/bin/sshd-host", "//garnet/bin/sshd-host:config", "//garnet/bin/sysmgr", "//garnet/bin/sysmgr:network_config", "//garnet/bin/sysmgr:services_config", "//garnet/bin/timezone", "//src/cobalt/bin/app:cobalt", "//src/cobalt/bin/app:cobalt_registry", "//src/cobalt/bin/app:config", "//src/cobalt/bin/system-metrics:cobalt_system_metrics", "//src/connectivity/bluetooth:core", "//src/connectivity/management:network_config_default", "//src/connectivity/network/mdns/bundles:config", "//src/connectivity/network/mdns/bundles:services", "//src/connectivity/network/net-cli", "//src/connectivity/network:config", "//src/connectivity/wlan:packages", "//src/connectivity/wlan/config:default", "//src/developer/exception_broker", "//src/developer/feedback/bugreport", "//src/developer/feedback/crash_reports:crash-reports", "//src/developer/feedback/feedback_data:feedback_agent", "//src/developer/feedback/last_reboot:last-reboot", "//src/developer/remote-control:pkg", "//src/diagnostics/archivist", "//src/diagnostics/archivist:with_default_config", "//src/hwinfo:hwinfo", "//src/hwinfo:default_product_config", "//src/media/audio/bundles:audio_config", "//src/recovery/factory_reset", "//src/security/policy:appmgr_policy_eng", "//src/security/root_ssl_certificates", "//src/sys/appmgr", "//src/sys/appmgr:appmgr_scheme_config", "//src/sys/appmgr:core_component_id_index", "//src/sys/core", "//src/sys/device_settings:device_settings_manager", "//src/sys/pkg:core", "//src/sys/pkg:pkgfs-disable-executability-restrictions", "//src/sys/pkg:system-update-checker", "//src/sys/pkg/bin/pkg-resolver:enable_dynamic_configuration", "//src/sys/stash:pkg", "//src/sys/timekeeper", "//third_party/openssh-portable/fuchsia/developer-keys:ssh_config", "//src/sys/pkg:tools", "//tools/cargo-gnaw", "//bundles:kitchen_sink"]`

From //root_build_dir/args.gn:3

**Overridden from the default:** `[]`

From //BUILD.gn:25

**Current value for `target_cpu = "x64"`:** `["//build/info:build-info", "//garnet/bin/http", "//src/connectivity/network/http_client", "//garnet/bin/log_listener:log_listener", "//garnet/bin/log_listener:log_listener_shell", "//garnet/bin/network_time_service", "//garnet/bin/scpi", "//garnet/bin/setui:setui_service", "//garnet/bin/sshd-host", "//garnet/bin/sshd-host:config", "//garnet/bin/sysmgr", "//garnet/bin/sysmgr:network_config", "//garnet/bin/sysmgr:services_config", "//garnet/bin/timezone", "//src/cobalt/bin/app:cobalt", "//src/cobalt/bin/app:cobalt_registry", "//src/cobalt/bin/app:config", "//src/cobalt/bin/system-metrics:cobalt_system_metrics", "//src/connectivity/bluetooth:core", "//src/connectivity/management:network_config_default", "//src/connectivity/network/mdns/bundles:config", "//src/connectivity/network/mdns/bundles:services", "//src/connectivity/network/net-cli", "//src/connectivity/network:config", "//src/connectivity/wlan:packages", "//src/connectivity/wlan/config:default", "//src/developer/exception_broker", "//src/developer/feedback/bugreport", "//src/developer/feedback/crash_reports:crash-reports", "//src/developer/feedback/feedback_data:feedback_agent", "//src/developer/feedback/last_reboot:last-reboot", "//src/developer/remote-control:pkg", "//src/diagnostics/archivist", "//src/diagnostics/archivist:with_default_config", "//src/hwinfo:hwinfo", "//src/hwinfo:default_product_config", "//src/media/audio/bundles:audio_config", "//src/recovery/factory_reset", "//src/security/policy:appmgr_policy_eng", "//src/security/root_ssl_certificates", "//src/sys/appmgr", "//src/sys/appmgr:appmgr_scheme_config", "//src/sys/appmgr:core_component_id_index", "//src/sys/core", "//src/sys/device_settings:device_settings_manager", "//src/sys/pkg:core", "//src/sys/pkg:pkgfs-disable-executability-restrictions", "//src/sys/pkg:system-update-checker", "//src/sys/pkg/bin/pkg-resolver:enable_dynamic_configuration", "//src/sys/stash:pkg", "//src/sys/timekeeper", "//third_party/openssh-portable/fuchsia/developer-keys:ssh_config", "//src/sys/pkg:tools", "//tools/cargo-gnaw", "//bundles:kitchen_sink"]`

From //root_build_dir/args.gn:3

**Overridden from the default:** `[]`

From //BUILD.gn:25

### blob_blobfs_maximum_bytes
For build/images:fvm.blob.sparse.blk, use this argument.

**Current value (from the default):** `""`

From //build/images/fvm.gni:70

### blob_blobfs_minimum_data_bytes
For build/images:fvm.blob.sparse.blk, use this argument.

**Current value (from the default):** `""`

From //build/images/fvm.gni:56

### blob_blobfs_minimum_inodes
For build/images:fvm.blob.sparse.blk, use this argument.

**Current value (from the default):** `""`

From //build/images/fvm.gni:45

### blobfs_maximum_bytes
In addition to reserving space for inodes and data, fs needs additional
space for maintaining some internal data structures. So the
space required to reserve inodes and data may exceed sum of the space
needed for inodes and data.
maximum_bytes puts an upper bound on the total bytes reserved for inodes,
data bytes and reservation for all other internal fs metadata.
An empty string does not put any upper bound. A filesystem may
reserve few blocks required for its operations.

**Current value (from the default):** `""`

From //build/images/fvm.gni:66

### blobfs_minimum_data_bytes
Number of bytes to reserve for data in the fs. This is in addition
to what is reserved, if any, for the inodes. Data bytes constitutes
"usable" space of the fs.
An empty string does not reserve any additional space than minimum
required for the filesystem.

**Current value (from the default):** `""`

From //build/images/fvm.gni:52

### blobfs_minimum_inodes
minimum_inodes is the number of inodes to reserve for the fs
An empty string does not reserve any additional space than minimum
required for the filesystem.

**Current value (from the default):** `""`

From //build/images/fvm.gni:41

### board_bootfs_labels
A list of bootfs manifest labels to include in the ZBI.

**Current value for `target_cpu = "arm64"`:** `["//garnet/bin/power_manager/node_config:base.manifest"]`

From //boards/arm64.gni:18

**Overridden from the default:** `[]`

From //build/board.gni:14

**Current value for `target_cpu = "x64"`:** `["//garnet/bin/power_manager/node_config:base.manifest"]`

From //boards/x64.gni:19

**Overridden from the default:** `[]`

From //build/board.gni:14

### board_extra_vbmeta_images
Board level extra vbmeta images to be combined into the top-level vbmeta
struct.

**Current value (from the default):** `[]`

From //build/images/vbmeta.gni:36

### board_has_libvulkan_arm_mali
Board files can set this to true if they have a package with a mali libvulkan VCD.

**Current value (from the default):** `false`

From //src/graphics/lib/magma/gnbuild/magma.gni:49

### board_kernel_cmdline_args
List of kernel command line this board to bake into the boot image that are
required by this board. See also kernel_cmdline_args in
//build/images/BUILD.gn

**Current value (from the default):** `[]`

From //build/board.gni:19

### board_name
Board name used for paving and amber updates.

**Current value for `target_cpu = "arm64"`:** `"qemu-arm64"`

From //boards/arm64.gni:7

**Overridden from the default:** `""`

From //build/board.gni:7

**Current value for `target_cpu = "x64"`:** `"x64"`

From //boards/x64.gni:7

**Overridden from the default:** `""`

From //build/board.gni:7

### board_package_labels
A list of package labels to include in the 'base' package set. Used by the
board definition rather than the product definition.

**Current value for `target_cpu = "arm64"`:** `["//garnet/bin/thermd", "//garnet/bin/thermd:config", "//garnet/packages/prod:drivers", "//src/media/audio/bundles:virtual_audio_driver"]`

From //boards/arm64.gni:9

**Overridden from the default:** `[]`

From //build/board.gni:11

**Current value for `target_cpu = "x64"`:** `["//garnet/bin/thermd", "//garnet/bin/thermd:config", "//garnet/packages/prod:drivers", "//src/hwinfo:default_board_config", "//src/media/audio/bundles:virtual_audio_driver"]`

From //boards/x64.gni:9

**Overridden from the default:** `[]`

From //build/board.gni:11

### board_zedboot_cmdline_args
List of kernel command line arguments to bake into the zedboot image that are
required by this board. See also zedboot_cmdline_args in
//build/images/zedboot/BUILD.gn

**Current value (from the default):** `[]`

From //build/board.gni:24

### bootfs_allowlist_recovery
List of binaries to include in the bootfs manifest for recovery. This
overrides the option set by `bootfs_zircon_groups` so that only the requested
binaries are included in the final image.

**Current value (from the default):** `[]`

From //build/images/recovery/BUILD.gn:15

### bootfs_extra
List of extra manifest entries for files to add to the BOOTFS.
Each entry can be a "TARGET=SOURCE" string, or it can be a scope
with `sources` and `outputs` in the style of a copy() target:
`outputs[0]` is used as `TARGET` (see `gn help source_expansion`).

**Current value (from the default):** `[]`

From //build/images/args.gni:35

### bootfs_only
Put the "system image" package in the BOOTFS.  Hence what would
otherwise be /system/... at runtime is /boot/... instead.

**Current value for `target_cpu = "arm64"`:** `false`

From //products/core.gni:11

**Overridden from the default:** `false`

From //build/images/args.gni:14

**Current value for `target_cpu = "x64"`:** `false`

From //products/core.gni:11

**Overridden from the default:** `false`

From //build/images/args.gni:14

### bootloader_hw_revision
(deprecated)  HW revision of the bootloader to be included into OTA package
and paving process.

See `firmware_prebuilts_path_suffix` instead.

**Current value (from the default):** `""`

From //build/images/args.gni:53

### bootloader_prebuilt
(deprecated) Prebuilt bootloader image to be included into update (OTA)
package and paving process.

See `firmware_prebuilts` instead.

**Current value (from the default):** `""`

From //build/images/args.gni:47

### build_all_vp9_file_decoder_conformance_tests

**Current value (from the default):** `false`

From //src/media/codec/examples/BUILD.gn:10

### build_id_format
Build ID algorithm to use for Fuchsia-target code.  This does not apply
to host or guest code.  The value is the argument to the linker's
`--build-id=...` switch.  If left empty (the default), the linker's
default format is used.

**Current value (from the default):** `""`

From //build/config/fuchsia/BUILD.gn:16

### build_info_board
Board configuration of the current build

**Current value (from the default):** `"qemu-arm64"`

From //build/info/info.gni:12

### build_info_product
Product configuration of the current build

**Current value (from the default):** `""`

From //build/info/info.gni:9

### build_info_version
Logical version of the current build. If not set, defaults to the timestamp
of the most recent update.

**Current value (from the default):** `""`

From //build/info/info.gni:16

### build_libvulkan_arm_mali
Targets that will be built as mali vulkan ICDS.

**Current value (from the default):** `[]`

From //src/graphics/lib/magma/gnbuild/magma.gni:37

### build_libvulkan_goldfish
This is a list of targets that will be built as goldfish vulkan ICDs.

**Current value (from the default):** `[]`

From //src/graphics/lib/goldfish-vulkan/gnbuild/BUILD.gn:18

### build_libvulkan_img_rgx
Targets that will be built as IMG vulkan ICDS.

**Current value (from the default):** `[]`

From //src/graphics/lib/magma/gnbuild/magma.gni:46

### build_libvulkan_qcom_adreno
Targets that will be built as qualcomm vulkan ICDS.

**Current value (from the default):** `[]`

From //src/graphics/lib/magma/gnbuild/magma.gni:43

### build_libvulkan_vsl_gc
Targets that will be built as verisilicon vulkan ICDS.

**Current value (from the default):** `[]`

From //src/graphics/lib/magma/gnbuild/magma.gni:40

### build_sdk_archives
Whether to build SDK tarballs.

**Current value (from the default):** `false`

From //build/sdk/config.gni:7

### cache_package_labels
If you add package labels to this variable, the packages will be included
in the 'cache' package set, which represents an additional set of software
that is made available on disk immediately after paving and in factory
flows. These packages are not updated with an OTA, but instead are updated
ephemerally. This cache of software can be evicted by the system if storage
pressure arises or other policies indicate.

**Current value for `target_cpu = "arm64"`:** `[]`

From //products/core.gni:91

**Overridden from the default:** `[]`

From //BUILD.gn:33

**Current value for `target_cpu = "x64"`:** `[]`

From //products/core.gni:91

**Overridden from the default:** `[]`

From //BUILD.gn:33

### camera_debug

**Current value (from the default):** `false`

From //src/camera/debug.gni:6

### check_production_eligibility
Whether to perform check on the build's eligibility for production.
If true, base_packages and cache_packages are checked against dependencies
on //build/validate:non_production_tag, which is used to tag any
non-production GN labels. Build will fail if such dependency is found.

**Current value (from the default):** `false`

From //build/images/args.gni:108

### clang_lib_dir
Path to Clang lib directory.

**Current value (from the default):** `"../build/prebuilt/third_party/clang/linux-x64/lib"`

From //build/images/manifest.gni:9

### clang_prefix
The default clang toolchain provided by the prebuilt. This variable is
additionally consumed by the Go toolchain.

**Current value (from the default):** `"../prebuilt/third_party/clang/linux-x64/bin"`

From //build/config/clang/clang.gni:12

### cobalt_environment
Selects the Cobalt environment to send data to. Choices:
  "LOCAL" - record log data locally to a file
  "DEVEL" - the non-prod environment for use in testing
  "PROD" - the production environment

**Current value (from the default):** `"PROD"`

From //src/cobalt/bin/app/BUILD.gn:15

### compress_blobs
Whether to compress the blobfs image.

**Current value (from the default):** `true`

From //build/images/args.gni:111

### concurrent_dart_jobs
Maximum number of Dart processes to run in parallel.

Dart analyzer uses a lot of memory which may cause issues when building
with many parallel jobs e.g. when using goma. To avoid out-of-memory
errors we explicitly reduce the number of jobs.

**Current value (from the default):** `32`

From //build/dart/BUILD.gn:15

### concurrent_go_jobs
Maximum number of Go processes to run in parallel.

**Current value (from the default):** `32`

From //build/go/BUILD.gn:11

### concurrent_link_jobs
Maximum number of concurrent link jobs.

We often want to run fewer links at once than we do compiles, because
linking is memory-intensive. The default to use varies by platform and by
the amount of memory available, so we call out to a script to get the right
value.

**Current value (from the default):** `32`

From //build/toolchain/BUILD.gn:15

### concurrent_rust_jobs
Maximum number of Rust processes to run in parallel.

We run multiple rustc jobs in parallel, each of which can cause significant
amount of memory, especially when using LTO. To avoid out-of-memory errors
we explicitly reduce the number of jobs.

**Current value (from the default):** `32`

From //build/rust/BUILD.gn:15

### config_have_heap
Tells openweave to include files that require heap access.

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:32](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#32)

### crash_diagnostics_dir
Clang crash reports directory path. Use empty path to disable altogether.

**Current value (from the default):** `"//root_build_dir/clang-crashreports"`

From //build/config/BUILD.gn:13

### crashpad_dependencies

**Current value (from the default):** `"fuchsia"`

From [//third_party/crashpad/build/crashpad_buildconfig.gni:22](https://chromium.googlesource.com/crashpad/crashpad/+/95b4e6276836283a91e18382fb258598bd77f8aa/build/crashpad_buildconfig.gni#22)

### crashpad_use_boringssl_for_http_transport_socket

**Current value (from the default):** `true`

From [//third_party/crashpad/util/net/tls.gni:22](https://chromium.googlesource.com/crashpad/crashpad/+/95b4e6276836283a91e18382fb258598bd77f8aa/util/net/tls.gni#22)

### create_kernel_service_snapshot

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:104

### current_cpu

**Current value (from the default):** `""`

### current_os

**Current value (from the default):** `""`

### custom_signing_script
If non-empty, the given script will be invoked to produce a signed ZBI
image. The given script must accept -z for the input zbi path, and -o for
the output signed zbi path. The path must be in GN-label syntax (i.e.
starts with //).

**Current value (from the default):** `""`

From //build/images/custom_signing.gni:12

### dart_component_kind

**Current value (from the default):** `"static_library"`

From //third_party/dart/runtime/runtime_args.gni:80

### dart_core_snapshot_kind
Controls the kind of core snapshot linked into the standalone VM. Using a
core-jit snapshot breaks the ability to change various flags that affect
code generation.

**Current value (from the default):** `"core"`

From //third_party/dart/runtime/runtime_args.gni:56

### dart_custom_version_for_pub
When this argument is a non-empty string, the version repoted by the
Dart VM will be one that is compatible with pub's interpretation of
semantic version strings. The version string will also include the values
of the argument. In particular the version string will read:

    "M.m.p-dev.x.x-$(dart_custom_version_for_pub)-$(short_git_hash)"

Where 'M', 'm', and 'p' are the major, minor and patch version numbers,
and 'dev.x.x' is the dev version tag most recently preceeding the current
revision. The short git hash can be omitted by setting
dart_version_git_info=false

**Current value (from the default):** `""`

From //third_party/dart/runtime/runtime_args.gni:73

### dart_debug
Instead of using is_debug, we introduce a different flag for specifying a
Debug build of Dart so that clients can still use a Release build of Dart
while themselves doing a Debug build.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:9

### dart_debug_optimization_level
The optimization level to use for debug builds. Defaults to 0 for builds with
code coverage enabled.

**Current value (from the default):** `"2"`

From //third_party/dart/runtime/runtime_args.gni:36

### dart_default_app
Controls whether dart_app() targets generate JIT or AOT Dart snapshots.
This defaults to JIT, use `fx set <ARCH> --args
'dart_default_app="dart_aot_app"' to switch to AOT.

**Current value (from the default):** `"dart_jit_app"`

From [//topaz/runtime/dart/dart_component.gni:19](https://fuchsia.googlesource.com/topaz/+/740da6e876dc847bc66ca62f6c9f95939dc18b4d/runtime/dart/dart_component.gni#19)

### dart_enable_wasm
Whether dart:wasm should be enabled.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:94

### dart_force_product
Forces all Dart and Flutter apps to build in a specific configuration that
we use to build products.

**Current value (from the default):** `false`

From [//topaz/runtime/dart/config.gni:10](https://fuchsia.googlesource.com/topaz/+/740da6e876dc847bc66ca62f6c9f95939dc18b4d/runtime/dart/config.gni#10)

### dart_lib_export_symbols
Whether libdart should export the symbols of the Dart API.

**Current value (from the default):** `true`

From //third_party/dart/runtime/runtime_args.gni:91

### dart_platform_bytecode
Controls whether the VM uses bytecode.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:84

### dart_runtime_mode
Set the runtime mode. This affects how the runtime is built and what
features it has. Valid values are:
'develop' (the default) - VM is built to run as a JIT with all development
features enabled.
'profile' - The VM is built to run with AOT compiled code with only the
CPU profiling features enabled.
'release' - The VM is built to run with AOT compiled code with no developer
features enabled.

These settings are only used for Flutter, at the moment. A standalone build
of the Dart VM should leave this set to "develop", and should set
'is_debug', 'is_release', or 'is_product'.

TODO(rmacnak): dart_runtime_mode no longer selects whether libdart is build
for JIT or AOT, since libdart waw split into libdart_jit and
libdart_precompiled_runtime. We should remove this flag and just set
dart_debug/dart_product.

**Current value (from the default):** `"develop"`

From //third_party/dart/runtime/runtime_args.gni:28

### dart_snapshot_kind

**Current value (from the default):** `"kernel"`

From //third_party/dart/utils/application_snapshot.gni:15

### dart_space_dart
Whether experimental space dart mode is enabled for Dart applications.

**Current value (from the default):** `false`

From [//topaz/runtime/dart/dart_component.gni:35](https://fuchsia.googlesource.com/topaz/+/740da6e876dc847bc66ca62f6c9f95939dc18b4d/runtime/dart/dart_component.gni#35)

### dart_target_arch
Explicitly set the target architecture to use a simulator.
Available options are: arm, arm64, x64, ia32.

**Current value (from the default):** `"arm64"`

From //third_party/dart/runtime/runtime_args.gni:32

### dart_use_crashpad
Whether to link Crashpad library for crash handling. Only supported on
Windows for now.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:51

### dart_use_fallback_root_certificates
Whether to fall back to built-in root certificates when they cannot be
verified at the operating system level.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:43

### dart_use_tcmalloc
Whether to link the standalone VM against tcmalloc. The standalone build of
the VM enables this only for Linux builds.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:47

### dart_version_git_info
Whether the Dart binary version string should include the git hash and
git commit time.

**Current value (from the default):** `true`

From //third_party/dart/runtime/runtime_args.gni:60

### dart_vm_code_coverage
Whether to enable code coverage for the standalone VM.

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:39

### data_partition_manifest
Path to manifest file containing data to place into the initial /data
partition.

**Current value (from the default):** `""`

From //build/images/args.gni:72

### debian_guest_earlycon

**Current value (from the default):** `false`

From //src/virtualization/packages/debian_guest/BUILD.gn:10

### debian_guest_qcow
Package the rootfs as a QCOW image (as opposed to a flat file).

**Current value (from the default):** `true`

From //src/virtualization/packages/debian_guest/BUILD.gn:9

### debug_zircon_libraries_more
Use this flag to optimize source libraries imported from Zircon the same was
as other libraries in this build.
By default, they are optimized the same as in the ZN build, which leaves
less debugging information available.

**Current value (from the default):** `false`

From //build/unification/config/BUILD.gn:10

### default_git_folder
Absolute path to the .git folder.

This is used in rules that need to refer to `.git/logs/HEAD` to include
a hash in the version string. By default the folder is `.git`, but we define
it as an argument so it can be overriden by users of `git-worktree` (See
Issue #33619).

When using git-worktree, you can add

   default_git_folder = "/path/to/main/git/repo/.git/worktrees/name/"

to out/ReleaseX64/args.gn. The path above can be extracted from the `.git`
file under the git worktree folder.

**Current value (from the default):** `"//third_party/dart//.git"`

From //third_party/dart/sdk_args.gni:27

### devmgr_config
List of arguments to add to /boot/config/devmgr.
These come after synthesized arguments to configure blobfs and pkgfs.

**Current value (from the default):** `[]`

From //build/images/args.gni:18

### dont_use_nnbd
Whether to build a Legacy SDK using Legacy core libraries.
TODO(38701): Remove dont_use_nnbd once the NNBD SDK is stable/performant
and there is no need to build a legacy version of the SDK for comparison
purposes.

**Current value (from the default):** `false`

From //third_party/dart/sdk_args.gni:12

### enable_api_diff
Detect dart API changes
TODO(fxb/36723, fxb/6623) Remove this flag once issues are resolved

**Current value (from the default):** `false`

From //build/dart/dart_library.gni:17

### enable_dart_analysis
Enable all dart analysis

**Current value (from the default):** `true`

From //build/dart/dart_library.gni:13

### enable_frame_pointers
Controls whether the compiler emits full stack frames for function calls.
This reduces performance but increases the ability to generate good
stack traces, especially when we have bugs around unwind table generation.
It applies only for Fuchsia targets (see below where it is unset).

TODO(ZX-2361): Theoretically unwind tables should be good enough so we can
remove this option when the issues are addressed.

**Current value (from the default):** `true`

From //build/config/BUILD.gn:23

### enable_gfx_subsystem

**Current value (from the default):** `true`

From //src/ui/scenic/bin/BUILD.gn:11

### enable_input_subsystem

**Current value (from the default):** `true`

From //src/ui/scenic/bin/BUILD.gn:12

### enable_mdns_trace
Enables the tracing feature of mdns, which can be turned on using
"mdns-util verbose".

**Current value (from the default):** `false`

From //src/connectivity/network/mdns/service/BUILD.gn:15

### enable_netboot
Whether to build the netboot zbi by default.

You can still build //build/images:netboot explicitly even if enable_netboot is false.

**Current value (from the default):** `false`

From //build/images/args.gni:77

### escher_test_for_glsl_spirv_mismatch
If true, this enables the |SpirvNotChangedTest| to check if the precompiled
shaders on disk are up to date and reflect the current shader source code
compiled with the latest shaderc tools/optimizations. People on the Scenic
team should build with this flag turned on to make sure that any shader
changes that were not run through the precompiler have their updated spirv
written to disk. Other teams and CQ do not need to worry about this flag.

**Current value (from the default):** `false`

From //src/ui/lib/escher/build_args.gni:26

### escher_use_null_vulkan_config_on_host
Using Vulkan on host (i.e. Linux) is an involved affair that involves
downloading the Vulkan SDK, setting environment variables, and so forth...
all things that are difficult to achieve in a CQ environment.  Therefore,
by default we use a stub implementation of Vulkan which fails to create a
VkInstance.  This allows everything to build, and also allows running Escher
unit tests which don't require Vulkan.

**Current value (from the default):** `true`

From //src/ui/lib/escher/build_args.gni:12

### escher_use_runtime_glsl
Determines whether or not escher will build with the glslang and shaderc
libraries. When false, these libraries will not be included in the scenic/
escher binary and as a result shaders will not be able to be compiled at
runtime. Precompiled spirv code will be loaded into memory from disk instead.

**Current value (from the default):** `false`

From //src/ui/lib/escher/build_args.gni:18

### exclude_kernel_service
Whether the VM includes the kernel service in all modes (debug, release,
product).

**Current value (from the default):** `false`

From //third_party/dart/runtime/runtime_args.gni:88

### exclude_testonly_syscalls
If true, excludes syscalls with the [testonly] attribute.

**Current value (from the default):** `false`

From //zircon/vdso/vdso.gni:7

### expat_build_root

**Current value (from the default):** `"//third_party/expat"`

From //src/graphics/lib/magma/gnbuild/magma.gni:14

### experimental_wlan_client_mlme
Selects the SoftMAC client implementation to use. Choices:
  false (default) - C++ Client MLME implementation
  true - Rust Client MLME implementation
This argument is temporary until Rust MLME is ready to be used.

**Current value (from the default):** `false`

From //src/connectivity/wlan/lib/mlme/cpp/BUILD.gn:10

### extra_manifest_args
Extra args to globally apply to the manifest generation script.

**Current value (from the default):** `[]`

From //build/images/manifest.gni:12

### extra_package_labels

**Current value (from the default):** `[]`

From //third_party/cobalt/BUILD.gn:9

### extra_variants
Additional variant toolchain configs to support.
This is just added to [`known_variants`](#known_variants).

**Current value (from the default):** `[]`

From //build/config/BUILDCONFIG.gn:653

### fastboot_product

**Current value (from the default):** `""`

From //build/images/args.gni:91

### fidl_write_v1_wireformat
TODO(fxb/41298): This is a temporary change to activate writing the
v1 FIDL wire-format selectively. Remove this when all bindings start
writing v1 wire-format by default.

**Current value (from the default):** `true`

From //build/fidl/wireformat.gni:9

### fidlc_deprecate_c_unions

**Current value (from the default):** `false`

From //zircon/tools/fidl/BUILD.gn:7

### fidlc_union_not_simple

**Current value (from the default):** `false`

From //zircon/tools/fidl/BUILD.gn:6

### filter_out_of_astro
Use this flag to exclude artifacts from Astro builds.
This is a very hacky way of ensuring that the resulting fuchsia.zbi fits
within the available space on device.
This will soon enough be replaced with a proper way to select image
artifacts based on board / product properties.
Artifacts that are known to not be useful on Astro builds (e.g. drivers for
other devices, tests) may be put in `if (!filter_out_of_astro)` blocks.
TODO(45680): remove this hack.

**Current value (from the default):** `false`

From //build/unification/images/BUILD.gn:14

### firmware_prebuilts
List of prebuilt firmware blobs to include in update packages.

Each entry in the list is a scope defining `path` and `type`. A build can
only have a single firmware blob of each `type`.

Note that `firmware_prebuilts_path_suffix` will be automatically appended to
all `path` variables, so do not include the suffix here.

**Current value (from the default):** `[]`

From //build/images/args.gni:62

### firmware_prebuilts_path_suffix
Suffix to append to all `firmware_prebuilts` paths.

Typically this indicates the hardware revision, and is made available so
that users can easily switch revisions using a single arg.

**Current value (from the default):** `""`

From //build/images/args.gni:68

### flutter_default_app

**Current value (from the default):** `"flutter_jit_app"`

From [//topaz/runtime/dart/dart_component.gni:12](https://fuchsia.googlesource.com/topaz/+/740da6e876dc847bc66ca62f6c9f95939dc18b4d/runtime/dart/dart_component.gni#12)

### flutter_driver_enabled
Enables/Disables flutter driver using '--args=flutter_driver_enabled=[true/false]'
in fx set. (Disabled by default)
This is effective only on debug builds.

**Current value (from the default):** `false`

From //build/testing/flutter_driver.gni:9

### flutter_profile

**Current value (from the default):** `true`

From [//topaz/runtime/dart/dart_component.gni:26](https://fuchsia.googlesource.com/topaz/+/740da6e876dc847bc66ca62f6c9f95939dc18b4d/runtime/dart/dart_component.gni#26)

### flutter_space_dart
Whether experimental space dart mode is enabled for Flutter applications.

**Current value (from the default):** `false`

From [//topaz/runtime/dart/dart_component.gni:32](https://fuchsia.googlesource.com/topaz/+/740da6e876dc847bc66ca62f6c9f95939dc18b4d/runtime/dart/dart_component.gni#32)

### font_catalog_paths

**Current value (from the default):** `["//prebuilt/third_party/fonts/fuchsia.font_catalog.json"]`

From //src/fonts/build/font_args.gni:17

### font_pkg_entries
Merged contents of .font_pkgs.json files. Populated in fonts.gni.

**Current value (from the default):** `[]`

From //src/fonts/build/font_args.gni:32

### font_pkgs_paths
Locations of .font_pkgs.json files, which list the locations of font files
within the workspace, as well as safe names that are derived from the fonts'
file names and can be used to name Fuchsia packages.

**Current value (from the default):** `["//prebuilt/third_party/fonts/fuchsia.font_pkgs.json"]`

From //src/fonts/build/font_args.gni:22

### fonts_dir
Directory into which all fonts are checked out from CIPD

**Current value (from the default):** `"//prebuilt/third_party/fonts"`

From //src/fonts/build/font_args.gni:12

### fuchsia_sdk_root
Consumers of the Fuchsia SDK instantiate templates for various SDK parts at
a specific spot within their buildroots. The target name for the specific
part is then derived from the part name as specified in the meta.json
manifest. Different buildroot instantiate the SDK parts at different
locations and then set this variable. GN rules can then prefix this variable
name in SDK builds to the name of the SDK part. This flag is meaningless in
non-SDK buildroots.

**Current value (from the default):** `""`

From //build/fuchsia/sdk.gni:17

### fuchsia_ta_uuids
UUID of TAs to include in the Fuchsia build.

**Current value (from the default):** `[]`

From //build/images/ta.gni:10

### fvm_image_size
The size in bytes of the FVM partition image to create. Normally this is
computed to be just large enough to fit the blob and data images. The
default value is "", which means to size based on inputs. Specifying a size
that is too small will result in build failure.

**Current value (from the default):** `""`

From //build/images/fvm.gni:12

### fvm_max_disk_size
The max size of the disk where the FVM is written. This is used for
preallocating metadata to determine how much the FVM can expand on disk.
Only applies to sparse FVM images. At sparse image construction time, the
build fails if the inputs are larger than `fvm_max_disk_size`. At paving
time, the FVM will be sized to the target's disk size up to
`fvm_max_disk_size`. If the size of the disk increases after initial paving,
the FVM will resize up to `fvm_max_disk_size`. During paving, if the target
FVM has declared a smaller size than `fvm_max_disk_size`, the FVM is
reinitialized to the larger size.
The default value is "" which sets the max disk size to the size of the disk
at pave/format time.

**Current value (from the default):** `""`

From //build/images/fvm.gni:25

### fvm_slice_size
The size of the FVM partition images "slice size". The FVM slice size is a
minimum size of a particular chunk of a partition that is stored within
FVM. A very small slice size may lead to decreased throughput. A very large
slice size may lead to wasted space. The selected default size of 8mb is
selected for conservation of space, rather than performance.

**Current value (from the default):** `"8388608"`

From //build/images/fvm.gni:32

### glm_build_root

**Current value (from the default):** `"//third_party/glm"`

From //src/graphics/lib/magma/gnbuild/magma.gni:17

### go_vet_enabled
  go_vet_enabled
    [bool] if false, go vet invocations are disabled for all builds.

**Current value (from the default):** `false`

From //build/go/go_build.gni:21

### gocache_dir
  gocache_dir
    Directory GOCACHE environment variable will be set to. This directory
    will have build and test results cached, and is safe to be written to
    concurrently. If overridden, this directory must be a full path.

**Current value (from the default):** `"/b/s/w/ir/k/root_build_dir/fidling/.gocache"`

From //build/go/go_build.gni:17

### goma_dir
Directory containing the Goma source code.  This can be a GN
source-absolute path ("//...") or a system absolute path.

**Current value (from the default):** `"//prebuilt/third_party/goma/linux-x64"`

From //build/toolchain/goma.gni:15

### graphics_compute_generate_debug_shaders
Set to true in your args.gn file to generate pre-processed and
auto-formatted shaders under the "debug" sub-directory of HotSort
and Spinel target generation output directories.

These are never used, but can be reviewed manually to verify the
impact of configuration parameters, or when modifying a compute
shader.

Example results:

  out/default/
    gen/src/graphics/lib/compute/
       hotsort/targets/hs_amd_gcn3_u64/
          comp/
            hs_transpose.comp -> unpreprocessed shader
          debug/
            hs_transpose.glsl -> preprocessed shader


**Current value (from the default):** `true`

From //src/graphics/lib/compute/gn/glsl_shader_rules.gni:28

### graphics_compute_skip_spirv_opt
At times we may want to compare the performance of unoptimized
vs. optimized shaders.  On desktop platforms, use of spirv-opt
doesn't appear to provide major performance improvements but it
significantly reduces the size of the SPIR-V modules.

Disabling the spirv-opt pass may also be useful in identifying and
attributing code generation bugs.


**Current value (from the default):** `true`

From //src/graphics/lib/compute/gn/glsl_shader_rules.gni:38

### graphics_compute_verbose_compile
The glslangValidator compiler is noisy by default.  A cleanly
compiling shader still prints out its filename.

This negatively impacts the GN build.

For this reason, we silence the compiler with the "-s" option but
unfortunately this also disables all error reporting.

Set to true to see detailed error reporting.


**Current value (from the default):** `false`

From //src/graphics/lib/compute/gn/glsl_shader_rules.gni:50

### host_byteorder

**Current value (from the default):** `"undefined"`

From //build/config/host_byteorder.gni:7

### host_cpu

**Current value (from the default):** `"x64"`

### host_os

**Current value (from the default):** `"linux"`

### host_tools_dir
This is the directory where host tools intended for manual use by
developers get installed.  It's something a developer might put
into their shell's $PATH.  Host tools that are just needed as part
of the build do not get copied here.  This directory is only for
things that are generally useful for testing or debugging or
whatnot outside of the GN build itself.  These are only installed
by an explicit install_host_tools() rule (see //build/host.gni).

**Current value (from the default):** `"//root_build_dir/host-tools"`

From //build/host.gni:13

### icu_use_data_file
Tells icu to load an external data file rather than rely on the icudata
being linked directly into the binary.

**Current value (from the default):** `true`

From [//third_party/icu/config.gni:8](https://fuchsia.googlesource.com/third_party/icu/+/995c0373ab900da6cbf97cb3074d9f475efaa915/config.gni#8)

### include_devmgr_config_in_vbmeta
If true, /config/devmgr config will be included into a vbmeta image
instead of bootfs.

**Current value (from the default):** `false`

From //build/images/vbmeta.gni:14

### include_fvm_blob_sparse
Include fvm.blob.sparse.blk image into the build if set to true

**Current value (from the default):** `false`

From //build/images/args.gni:121

### include_internal_fonts
Set to true to include internal fonts in the build.

**Current value (from the default):** `false`

From //src/fonts/build/font_args.gni:7

### include_tests_that_fail_on_nuc_asan
Whether to include tests that are known to fail on NUC with ASan.
Should be set to false in the infra builders that have board == "x64" and
"asan" in variants.

**Current value (from the default):** `true`

From //build/testing/environments.gni:11

### include_zxdb_large_tests
Normally these tests are not built and run because they require large amounts of optional data
be downloaded. Set this to true to enable the build for the zxdb_large_tests.
See symbols/test_data/README.md for how to download the data required for this test.

**Current value (from the default):** `false`

From //src/developer/debug/zxdb/BUILD.gn:13

### inet_config_enable_async_dns_sockets
Tells inet to support additionally support async dns sockets.

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:17](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#17)

### inet_want_endpoint_dns
Tells inet to include support for the corresponding protocol.

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:10](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#10)

### inet_want_endpoint_raw

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:11](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#11)

### inet_want_endpoint_tcp

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:12](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#12)

### inet_want_endpoint_tun

**Current value (from the default):** `false`

From [//third_party/openweave-core/config.gni:14](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#14)

### inet_want_endpoint_udp

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:13](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#13)

### is_debug
Debug build.

**Current value (from the default):** `true`

From //build/config/BUILDCONFIG.gn:25

### kernel_cmdline_args
List of kernel command line arguments to bake into the boot image.
See also [kernel_cmdline](/docs/reference/kernel/kernel_cmdline.md) and
[`devmgr_config`](#devmgr_config).

**Current value for `target_cpu = "arm64"`:** `["console.shell=true", "kernel.enable-debugging-syscalls=true", "kernel.enable-serial-syscalls=true", "netsvc.all-features=true", "netsvc.disable=false", "kernel.oom.behavior=reboot"]`

From //products/core.gni:17

**Overridden from the default:** `[]`

From //build/images/args.gni:23

**Current value for `target_cpu = "x64"`:** `["console.shell=true", "kernel.enable-debugging-syscalls=true", "kernel.enable-serial-syscalls=true", "netsvc.all-features=true", "netsvc.disable=false", "kernel.oom.behavior=reboot"]`

From //products/core.gni:17

**Overridden from the default:** `[]`

From //build/images/args.gni:23

### kernel_cmdline_files
Files containing additional kernel command line arguments to bake into
the boot image.  The contents of these files (in order) come after any
arguments directly in [`kernel_cmdline_args`](#kernel_cmdline_args).
These can be GN `//` source pathnames or absolute system pathnames.

**Current value (from the default):** `[]`

From //build/images/args.gni:29

### known_variants
List of variants that will form the basis for variant toolchains.
To make use of a variant, set [`select_variant`](#select_variant).

Normally this is not set as a build argument, but it serves to
document the available set of variants.
See also [`universal_variants`](#universal_variants).
Only set this to remove all the default variants here.
To add more, set [`extra_variants`](#extra_variants) instead.

Each element of the list is one variant, which is a scope defining:

  `configs` (optional)
      [list of labels] Each label names a config that will be
      automatically used by every target built in this variant.
      For each config `${label}`, there must also be a target
      `${label}_deps`, which each target built in this variant will
      automatically depend on.  The `variant()` template is the
      recommended way to define a config and its `_deps` target at
      the same time.

  `remove_common_configs` (optional)
  `remove_shared_configs` (optional)
      [list of labels] This list will be removed (with `-=`) from
      the `default_common_binary_configs` list (or the
      `default_shared_library_configs` list, respectively) after
      all other defaults (and this variant's configs) have been
      added.

  `deps` (optional)
      [list of labels] Added to the deps of every target linked in
      this variant (as well as the automatic `${label}_deps` for
      each label in configs).

  `name` (required if configs is omitted)
      [string] Name of the variant as used in
      [`select_variant`](#select_variant) elements' `variant` fields.
      It's a good idea to make it something concise and meaningful when
      seen as e.g. part of a directory name under `$root_build_dir`.
      If name is omitted, configs must be nonempty and the simple names
      (not the full label, just the part after all `/`s and `:`s) of these
      configs will be used in toolchain names (each prefixed by a "-"),
      so the list of config names forming each variant must be unique
      among the lists in `known_variants + extra_variants`.

  `toolchain_args` (optional)
      [scope] Each variable defined in this scope overrides a
      build argument in the toolchain context of this variant.

  `host_only` (optional)
  `target_only` (optional)
      [scope] This scope can contain any of the fields above.
      These values are used only for host or target, respectively.
      Any fields included here should not also be in the outer scope.


**Current value (from the default):**
```
[{
  configs = ["//build/config/lto"]
}, {
  configs = ["//build/config/lto:thinlto"]
}, {
  configs = ["//build/config/profile"]
  instrumented = true
}, {
  configs = ["//build/config/scudo"]
}, {
  configs = ["//build/config/sanitizers:ubsan"]
  instrumented = true
}, {
  configs = ["//build/config/sanitizers:ubsan", "//build/config/sanitizers:sancov"]
  instrumented = true
}, {
  configs = ["//build/config/sanitizers:asan"]
  host_only = {
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
}
  instrumented = true
  toolchain_args = {
  use_scudo = false
}
}, {
  configs = ["//build/config/sanitizers:asan", "//build/config/sanitizers:ubsan"]
  host_only = {
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
}
  instrumented = true
  toolchain_args = {
  use_scudo = false
}
}, {
  configs = ["//build/config/sanitizers:asan", "//build/config/sanitizers:sancov"]
  host_only = {
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
}
  instrumented = true
  toolchain_args = {
  use_scudo = false
}
}, {
  configs = ["//build/config/sanitizers:asan"]
  host_only = {
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
}
  instrumented = true
  name = "asan_no_detect_leaks"
  toolchain_args = {
  asan_default_options = "detect_leaks=0"
  use_scudo = false
}
}, {
  configs = ["//build/config/sanitizers:asan", "//build/config/sanitizers:rust-asan", "//build/config/sanitizers:fuzzer", "//build/config/fuchsia:icf"]
  host_only = {
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
}
  instrumented = true
  name = "asan-fuzzer"
  remove_common_configs = ["//build/config/fuchsia:icf"]
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
  toolchain_args = {
  asan_default_options = "alloc_dealloc_mismatch=0:check_malloc_usable_size=0:detect_odr_violation=0:max_uar_stack_size_log=16:print_scariness=1:allocator_may_return_null=1:detect_leaks=0:malloc_context_size=128:print_summary=1:print_suppressions=0:strict_memcmp=0:symbolize=0:clear_shadow_mmap_threshold=0"
  use_scudo = false
}
}, {
  configs = ["//build/config/sanitizers:ubsan", "//build/config/sanitizers:fuzzer", "//build/config/fuchsia:icf"]
  instrumented = true
  name = "ubsan-fuzzer"
  remove_common_configs = ["//build/config/fuchsia:icf"]
  remove_shared_configs = ["//build/config:symbol_no_undefined"]
}]
```

From //build/config/BUILDCONFIG.gn:578

### launch_basemgr_on_boot
Indicates whether to include basemgr.cmx in the boot sequence for the
product image.

**Current value (from the default):** `true`

From //src/modular/build/modular_config/modular_config.gni:11

### linux_guest_extras_path

**Current value (from the default):** `""`

From //src/virtualization/packages/linux_guest/BUILD.gn:12

### linux_runner_extras_tests
If `true`, adds additional testonly content to extras.img, which will be
built and mounted inside the container at /mnt/chromeos.

**Current value (from the default):** `false`

From //src/virtualization/packages/biscotti_guest/linux_runner/BUILD.gn:25

### linux_runner_gateway

**Current value (from the default):** `"10.0.0.1"`

From //src/virtualization/packages/biscotti_guest/linux_runner/BUILD.gn:20

### linux_runner_ip
Default values for the guest network configuration.

These are currently hard-coded to match what is setup in the virtio-net
device.

See //src/virtualization/bin/vmm/device/virtio_net.cc for more details.

**Current value (from the default):** `"10.0.0.2"`

From //src/virtualization/packages/biscotti_guest/linux_runner/BUILD.gn:19

### linux_runner_netmask

**Current value (from the default):** `"255.255.255.0"`

From //src/virtualization/packages/biscotti_guest/linux_runner/BUILD.gn:21

### linux_runner_volatile_block
If `true`, all block devices that would normally load as READ_WRITE will
be loaded as VOLATILE_WRITE. This is useful when working on changes to
the linux kernel as crashes and panics can sometimes corrupt the images.

**Current value (from the default):** `false`

From //src/virtualization/packages/biscotti_guest/linux_runner/BUILD.gn:30

### local_bench
Used to enable local benchmarking/fine-tuning when running benchmarks
in `fx shell`. Pass `--args=local_bench='true'` to `fx set` in order to
enable it.

**Current value (from the default):** `false`

From //src/developer/fuchsia-criterion/BUILD.gn:14

### log_startup_sleep

**Current value (from the default):** `"30000"`

From //garnet/bin/log_listener/BUILD.gn:15

### magma_build_root

**Current value (from the default):** `"//src/graphics/lib/magma"`

From //src/graphics/lib/magma/gnbuild/magma.gni:13

### magma_enable_developer_build
Enable this to have the msd include a suite of tests and invoke them
automatically when the driver starts.

**Current value (from the default):** `false`

From //src/graphics/lib/magma/gnbuild/magma.gni:27

### magma_enable_tracing
Enable this to include fuchsia tracing capability

**Current value (from the default):** `true`

From //src/graphics/lib/magma/gnbuild/magma.gni:23

### magma_python_path

**Current value (from the default):** `"/b/s/w/ir/k/third_party/mako"`

From //src/graphics/lib/magma/gnbuild/magma.gni:20

### max_blob_contents_size
Maximum allowable contents for the /blob in a release mode build.
Zero means no limit.
contents_size refers to contents stored within the filesystem (regardless
of how they are stored).

**Current value (from the default):** `"0"`

From //build/images/filesystem_limits.gni:10

### max_blob_image_size
Maximum allowable image_size for /blob in a release mode build.
Zero means no limit.
image_size refers to the total image size, including both contents and
metadata.

**Current value (from the default):** `"0"`

From //build/images/filesystem_limits.gni:16

### max_data_contents_size
Maximum allowable contents_size for /data in a release mode build.
Zero means no limit.
contents_size refers to contents stored within the filesystem (regardless
of how they are stored).

**Current value (from the default):** `"0"`

From //build/images/filesystem_limits.gni:22

### max_data_image_size
Maximum allowable image_size for /data in a release mode build.
Zero means no limit.
image_size refers to the total image size, including both contents and
metadata.

**Current value (from the default):** `"0"`

From //build/images/filesystem_limits.gni:28

### max_fuchsia_zbi_size
Maximum allowable size for fuchsia.zbi

**Current value for `target_cpu = "arm64"`:** `"16777216"`

From //boards/arm64.gni:20

**Overridden from the default:** `"0"`

From //build/images/filesystem_limits.gni:31

**Current value for `target_cpu = "x64"`:** `"16777216"`

From //boards/x64.gni:21

**Overridden from the default:** `"0"`

From //build/images/filesystem_limits.gni:31

### max_fvm_size
Maximum allowable size for the FVM in a release mode build
Zero means no limit

**Current value (from the default):** `"0"`

From //build/images/max_fvm_size.gni:8

### max_log_disk_usage
Controls how many bytes of space on disk are used to persist device logs.
Should be a string value that only contains digits.

**Current value (from the default):** `"0"`

From //garnet/bin/log_listener/BUILD.gn:14

### max_zedboot_zbi_size
Maximum allowable size for zedboot.zbi

**Current value for `target_cpu = "arm64"`:** `"16777216"`

From //boards/arm64.gni:21

**Overridden from the default:** `"0"`

From //build/images/filesystem_limits.gni:34

**Current value for `target_cpu = "x64"`:** `"16777216"`

From //boards/x64.gni:22

**Overridden from the default:** `"0"`

From //build/images/filesystem_limits.gni:34

### meta_package_labels
A list of labels for meta packages to be included in the monolith.

**Current value for `target_cpu = "arm64"`:** `["//build/images:config-data", "//build/images:shell-commands", "//src/sys/component_index:component_index"]`

From //products/core.gni:19

**Overridden from the default:** `[]`

From //build/images/args.gni:80

**Current value for `target_cpu = "x64"`:** `["//build/images:config-data", "//build/images:shell-commands", "//src/sys/component_index:component_index"]`

From //products/core.gni:19

**Overridden from the default:** `[]`

From //build/images/args.gni:80

### minfs_maximum_bytes

**Current value (from the default):** `""`

From //build/images/fvm.gni:67

### minfs_minimum_data_bytes

**Current value (from the default):** `""`

From //build/images/fvm.gni:53

### minfs_minimum_inodes

**Current value (from the default):** `""`

From //build/images/fvm.gni:42

### msd_arm_enable_all_cores
Enable all 8 cores, which is faster but emits more heat.

**Current value (from the default):** `true`

From //src/graphics/drivers/msd-arm-mali/src/BUILD.gn:9

### msd_arm_enable_cache_coherency
With this flag set the system tries to use cache coherent memory if the
GPU supports it.

**Current value (from the default):** `true`

From //src/graphics/drivers/msd-arm-mali/src/BUILD.gn:13

### msd_arm_enable_protected_debug_swap_mode
In protected mode, faults don't return as much information so they're much harder to debug. To
work around that, add a mode where protected atoms are executed in non-protected mode and
vice-versa.

NOTE: The memory security ranges should also be set (in TrustZone) to the opposite of normal, so
that non-protected mode accesses can only access protected memory and vice versa.  Also,
growable memory faults won't work in this mode, so larger portions of growable memory should
precommitted (which is not done by default).

**Current value (from the default):** `false`

From //src/graphics/drivers/msd-arm-mali/src/BUILD.gn:23

### msd_build_root

**Current value (from the default):** `"//src/graphics/drivers"`

From //src/graphics/lib/magma/gnbuild/magma.gni:15

### msd_intel_gen_build_root

**Current value (from the default):** `"//src/graphics/drivers/msd-intel-gen"`

From //src/graphics/lib/magma/gnbuild/magma.gni:16

### netcfg_autostart

**Current value (from the default):** `true`

From //src/connectivity/network/netcfg/BUILD.gn:12

### netsvc_extra_defines

**Current value (from the default):** `[]`

From //zircon/system/core/netsvc/BUILD.gn:19

### omaha_app_id
Default app id will always return no update.

**Current value (from the default):** `"fuchsia-test:no-update"`

From //src/sys/pkg/bin/omaha-client/BUILD.gn:14

### on_second_thought_keep_on_astro
Use this flag to include previously excluded artifacts on products based on
Astro.
Yes, our image building system is so broken that this is currently the only
way to accommodate the configurations we run on our infrastructure.

**Current value for `target_cpu = "arm64"`:** `false`

From //products/core.gni:9

**Overridden from the default:** `false`

From //build/unification/images/BUILD.gn:20

**Current value for `target_cpu = "x64"`:** `false`

From //products/core.gni:9

**Overridden from the default:** `false`

From //build/unification/images/BUILD.gn:20

### optimize
* `none`: really unoptimized, usually only build-tested and not run
* `debug`: "optimized for debugging", light enough to avoid confusion
* `default`: default optimization level
* `size`:  optimized for space rather than purely for speed
* `speed`: optimized purely for speed
* `sanitizer`: optimized for sanitizers (ASan, etc.)
* `profile`: optimized for coverage/profile data collection

**Current value (from the default):** `"debug"`

From //build/config/compiler.gni:33

### output_breakpad_syms
Sets if we should output breakpad symbols for Fuchsia binaries.

**Current value (from the default):** `false`

From //build/config/BUILDCONFIG.gn:28

### persist_logs

**Current value (from the default):** `false`

From //build/persist_logs.gni:13

### pkgfs_packages_allowlist

**Current value (from the default):** `"//src/security/policy/pkgfs_non_static_pkgs_allowlist_eng.txt"`

From //build/images/args.gni:118

### platform_enable_user_pci

**Current value (from the default):** `false`

From //src/devices/bus/drivers/pci/pci.gni:10

### pre_erase_flash

**Current value (from the default):** `false`

From //build/images/args.gni:92

### prebuilt_dart_sdk
Directory containing prebuilt Dart SDK.
This must have in its `bin/` subdirectory `gen_snapshot.OS-CPU` binaries.
Set to empty for a local build.

**Current value (from the default):** `"//prebuilt/third_party/dart/linux-x64"`

From //build/dart/dart.gni:9

### prebuilt_libvulkan_arm_path

**Current value (from the default):** `""`

From //src/graphics/lib/magma/gnbuild/magma.gni:29

### prebuilt_libvulkan_goldfish_path

**Current value (from the default):** `""`

From //src/graphics/lib/goldfish-vulkan/gnbuild/BUILD.gn:9

### prebuilt_libvulkan_img_path
The path to a prebuilt libvulkan.so for an IMG GPU.

**Current value (from the default):** `""`

From //src/graphics/lib/magma/gnbuild/magma.gni:32

### prototype_account_transfer
Whether or not prototype account transfer is enabled.
NOTE: This is not secure and should NOT be enabled for any products!  This
is only for use during local development.

**Current value (from the default):** `false`

From //src/identity/bin/account_manager/BUILD.gn:12

### recovery_ta_uuids
UUID of TAs to include in the Recovery build.

**Current value (from the default):** `[]`

From //build/images/ta.gni:16

### rust_cap_lints
Sets the maximum lint level.
"deny" will make all warnings into errors, "warn" preserves them as warnings, and "allow" will
ignore warnings.

**Current value (from the default):** `"deny"`

From //build/rust/config.gni:35

### rust_lto
Sets the default LTO type for rustc bulids.

**Current value (from the default):** `""`

From //build/rust/config.gni:27

### rust_override_lto
Overrides the LTO setting for all Rust builds, regardless of
debug/release flags or the `with_lto` arg to the rustc_ templates.
Valid values are "none", "thin", and "fat".

**Current value (from the default):** `""`

From //build/rust/config.gni:45

### rust_override_opt
Overrides the optimization level for all Rust builds, regardless of
debug/release flags or the `force_opt` arg to the rustc_ templates.
Valid values are 0-3, o, and z.

**Current value (from the default):** `""`

From //build/rust/config.gni:40

### rust_sysroot
Sets a custom base directory for where rust tooling
looks for the standard library

**Current value (from the default):** `"../prebuilt/third_party/rust/linux-x64"`

From //build/rust/config.gni:24

### rust_toolchain_triple_suffix
Sets the fuchsia toolchain target triple suffix (after arch)

**Current value (from the default):** `"fuchsia"`

From //build/rust/config.gni:30

### rustc_prefix
Sets a custom base directory for `rustc` and `cargo`.
This can be used to test custom Rust toolchains.

**Current value (from the default):** `"../prebuilt/third_party/rust/linux-x64/bin"`

From //build/rust/config.gni:20

### scenic_display_frame_number
Draws the current frame number in the top-left corner.

**Current value (from the default):** `false`

From //src/ui/scenic/lib/gfx/BUILD.gn:11

### scenic_enable_vulkan_validation
Include the vulkan validation layers in scenic.

**Current value (from the default):** `true`

From //src/ui/scenic/BUILD.gn:114

### scenic_ignore_vsync

**Current value (from the default):** `false`

From //src/ui/scenic/lib/gfx/BUILD.gn:8

### scudo_default_options
Default [Scudo](https://llvm.org/docs/ScudoHardenedAllocator.html)
options (before the `SCUDO_OPTIONS` environment variable is read at
runtime).  *NOTE:* This affects only components using the `scudo`
variant (see GN build argument `select_variant`), and does not affect
anything when the `use_scudo` build flag is set instead.

**Current value (from the default):** `["abort_on_error=1", "QuarantineSizeKb=0", "ThreadLocalQuarantineSizeKb=0", "DeallocationTypeMismatch=false", "DeleteSizeMismatch=false", "allocator_may_return_null=true"]`

From //build/config/scudo/scudo.gni:17

### sdk_dirs
The directories to search for parts of the SDK.

By default, we search the public directories for the various layers.
In the future, we'll search a pre-built SDK as well.

**Current value (from the default):** `["//garnet/public", "//topaz/public"]`

From //build/config/fuchsia/sdk.gni:10

### sdk_id
Identifier for the Core SDK.

**Current value (from the default):** `""`

From //sdk/config.gni:7

### select_variant
List of "selectors" to request variant builds of certain targets.
Each selector specifies matching criteria and a chosen variant.
The first selector in the list to match a given target determines
which variant is used for that target.

Each selector is either a string or a scope.  A shortcut selector is
a string; it gets expanded to a full selector.  A full selector is a
scope, described below.

A string selector can match a name in
[`select_variant_shortcuts`](#select_variant_shortcuts).  If it's not a
specific shortcut listed there, then it can be the name of any variant
described in [`known_variants`](#known_variants) and
[`universal_variants`](#universal_variants) (and combinations thereof).
A `selector` that's a simple variant name selects for every binary
built in the target toolchain: `{ host=false variant=selector }`.

If a string selector contains a slash, then it's `"shortcut/filename"`
and selects only the binary in the target toolchain whose `output_name`
matches `"filename"`, i.e. it adds `output_name=["filename"]` to each
selector scope that the shortcut's name alone would yield.

The scope that forms a full selector defines some of these:

    variant (required)
        [string or `false`] The variant that applies if this selector
        matches.  This can be `false` to choose no variant, or a string
        that names the variant.  See
        [`known_variants`](#known_variants) and
        [`universal_variants`](#universal_variants).

The rest below are matching criteria.  All are optional.
The selector matches if and only if all of its criteria match.
If none of these is defined, then the selector always matches.

The first selector in the list to match wins and then the rest of
the list is ignored.  To construct more complex rules, use a blocklist
selector with `variant=false` before a catch-all default variant, or
a list of specific variants before a catch-all false variant.

Each "[strings]" criterion is a list of strings, and the criterion
is satisfied if any of the strings matches against the candidate string.

    host
        [boolean] If true, the selector matches in the host toolchain.
        If false, the selector matches in the target toolchain.

    testonly
        [boolean] If true, the selector matches targets with testonly=true.
        If false, the selector matches in targets without testonly=true.

    target_type
        [strings]: `"executable"`, `"loadable_module"`, or `"driver_module"`

    output_name
        [strings]: target's `output_name` (default: its `target name`)

    label
        [strings]: target's full label with `:` (without toolchain suffix)

    name
        [strings]: target's simple name (label after last `/` or `:`)

    dir
        [strings]: target's label directory (`//dir` for `//dir:name`).

**Current value (from the default):** `[]`

From //build/config/BUILDCONFIG.gn:866

### select_variant_canonical
*This should never be set as a build argument.*
It exists only to be set in `toolchain_args`.
See //build/toolchain/clang_toolchain.gni for details.

**Current value (from the default):** `[]`

From //build/config/BUILDCONFIG.gn:871

### select_variant_shortcuts
List of short names for commonly-used variant selectors.  Normally this
is not set as a build argument, but it serves to document the available
set of short-cut names for variant selectors.  Each element of this list
is a scope where `.name` is the short name and `.select_variant` is a
a list that can be spliced into [`select_variant`](#select_variant).

**Current value (from the default):**
```
[{
  name = "host_asan"
  select_variant = [{
  dir = ["//third_party/yasm", "//third_party/vboot_reference", "//tools/vboot_reference", "//src/fonts/font_info"]
  host = true
  variant = "asan_no_detect_leaks"
}, {
  host = true
  variant = "asan"
}]
}, {
  name = "kasan"
  select_variant = []
}]
```

From //build/config/BUILDCONFIG.gn:699

### shaderc_enable_spvc_parser
Enables using the parsing built into spvc instead spirv-cross

**Current value (from the default):** `false`

From [//third_party/shaderc/shaderc_features.gni:17](https://fuchsia.googlesource.com/third_party/shaderc/+/ae50f26a6453fd8f8cd148fbd62a6ae9a94d4472/shaderc_features.gni#17)

### shaderc_spvc_disable_context_logging
Disables logging to messages in context struct

**Current value (from the default):** `false`

From [//third_party/shaderc/shaderc_features.gni:23](https://fuchsia.googlesource.com/third_party/shaderc/+/ae50f26a6453fd8f8cd148fbd62a6ae9a94d4472/shaderc_features.gni#23)

### shaderc_spvc_enable_direct_logging
Enables logging directly out to the terminal

**Current value (from the default):** `false`

From [//third_party/shaderc/shaderc_features.gni:20](https://fuchsia.googlesource.com/third_party/shaderc/+/ae50f26a6453fd8f8cd148fbd62a6ae9a94d4472/shaderc_features.gni#20)

### signed_image

**Current value (from the default):** `false`

From //build/images/args.gni:90

### size_checker_input
The input to the size checker.
The build system will produce a JSON file to be consumed by the size checker, which
will check and prevent integration of subsystems that are over their space allocation.
The input consists of the following keys:

asset_ext(string array): a list of extensions that should be considered as assets.

asset_limit(number): maximum size (in bytes) allocated for the assets.

core_limit(number): maximum size (in bytes) allocated for the core system and/or services.
This is sort of a "catch all" component that consists of all the area / packages that weren't
specified in the components list below.

components(object array): a list of component objects. Each object should contain the following keys:

  component(string): name of the component.

  src(string array): path of the area / package to be included as part of the component.
  The path should be relative to the obj/ in the output directory.
  For example, consider two packages foo and far, built to out/.../obj/some_big_component/foo and out/.../obj/some_big_component/bar.
  If you want to impose a limit on foo, your src will be ["some_big_component/foo"].
  If you want to impose a limit on both foo and far, your src will be ["some_big_component"].
  If a package has config-data, those prebuilt blobs actually live under the config-data package.
  If you wish to impose a limit of those data as well, you should add "build/images/config-data/$for_pkg" to your src.
  The $for_pkg corresponds to the $for_pkg field in config.gni.

  limit(number): maximum size (in bytes) allocated for the component.

Example:
size_checker_input = {
  asset_ext = [ ".ttf" ]
  asset_limit = 10240
  core_limit = 10240
  components = [
    {
      component = "Foo"
      src = [ "topaz/runtime/foo_runner" ]
      limit = 10240
    },
    {
      component = "Bar"
      src = [ "build/images" ]
      limit = 20480
    },
  ]
}

**Current value (from the default):** `{ }`

From //tools/size_checker/cmd/BUILD.gn:52

### symbol_level
How many symbols to include in the build. This affects the performance of
the build since the symbols are large and dealing with them is slow.
  2 means regular build with symbols.
  1 means minimal symbols, usually enough for backtraces only. Symbols with
internal linkage (static functions or those in anonymous namespaces) may not
appear when using this level.
  0 means no symbols.

**Current value (from the default):** `2`

From //build/config/compiler.gni:20

### syzkaller_dir
Used by syz-ci to build with own syz-executor source.

**Current value (from the default):** `"//third_party/syzkaller"`

From //src/testing/fuzzing/syzkaller/BUILD.gn:9

### ta_dest_suffix
File name suffix of TA images deployed on bootfs. Usually it is ".ta".
The TA image file names are "$ta_uuid$ta_dest_suffix".

**Current value (from the default):** `""`

From //build/images/ta.gni:24

### ta_path
Source absolute path to the prebuilt TA images.

**Current value (from the default):** `""`

From //build/images/ta.gni:7

### ta_src_suffix
File name suffix of prebuilt TA images. ".ta.prod" and ".ta.dev" are
usually used. The TA image file names are "$ta_uuid$ta_src_suffix".

**Current value (from the default):** `""`

From //build/images/ta.gni:20

### target_cpu

**Current value for `target_cpu = "arm64"`:** `"arm64"`

From //boards/arm64.gni:5

**Overridden from the default:** `""`

**Current value for `target_cpu = "x64"`:** `"x64"`

From //boards/x64.gni:5

**Overridden from the default:** `""`

### target_os

**Current value (from the default):** `""`

### target_sysroot
The absolute path of the sysroot that is used with the target toolchain.

**Current value (from the default):** `""`

From //build/config/sysroot.gni:7

### termina_disk
The termina disk image.

Defaults to the disk image from CIPD, but can be overridden to use a
custom disk for development purposes.

**Current value (from the default):** `"//prebuilt/virtualization/packages/termina_guest/images/arm64/vm_rootfs.img"`

From //src/virtualization/packages/termina_guest/BUILD.gn:18

### termina_kernel
The termina kernel image.

Defaults to the common linux kernel image from CIPD, but can be overridden to use a
custom kernel for development purposes.

**Current value (from the default):** `"//prebuilt/virtualization/packages/linux_guest/images/arm64/Image"`

From //src/virtualization/packages/termina_guest/BUILD.gn:12

### test_durations_file
A file in integration containing historical test duration data for this
build configuration. This file is used by infra to efficiently schedule
tests. "default.json" is a dummy file that contains no real duration data,
and causes infra to schedule tests as if each one has the same duration.

**Current value (from the default):** `"//integration/infra/test_durations/default.json"`

From //BUILD.gn:39

### thinlto_cache_dir
ThinLTO cache directory path.

**Current value (from the default):** `"dartlang/thinlto-cache"`

From //build/config/lto/config.gni:16

### thinlto_jobs
Number of parallel ThinLTO jobs.

**Current value (from the default):** `8`

From //build/config/lto/config.gni:13

### toolchain_variant
*This should never be set as a build argument.*
It exists only to be set in `toolchain_args`.
See //build/toolchain/clang_toolchain.gni for details.
This variable is a scope giving details about the current toolchain:
    `toolchain_variant.base`
        [label] The "base" toolchain for this variant, *often the
        right thing to use in comparisons, not `current_toolchain`.*
        This is the toolchain actually referenced directly in GN
        source code.  If the current toolchain is not
        `shlib_toolchain` or a variant toolchain, this is the same
        as `current_toolchain`.  In one of those derivative
        toolchains, this is the toolchain the GN code probably
        thought it was in.  This is the right thing to use in a test
        like `toolchain_variant.base == target_toolchain`, rather
        rather than comparing against `current_toolchain`.
    `toolchain_variant.name`
        [string] The name of this variant, as used in `variant` fields
        in [`select_variant`](#select_variant) clauses.  In the base
        toolchain and its `shlib_toolchain`, this is `""`.
    `toolchain_variant.suffix`
        [string] This is "-${toolchain_variant.name}", "" if name is empty.
    `toolchain_variant.is_pic_default`
        [bool] This is true in `shlib_toolchain`.
The other fields are the variant's effects as defined in
[`known_variants`](#known_variants).

**Current value (from the default):**
```
{
  base = "//build/toolchain/fuchsia:arm64"
}
```

From //build/config/BUILDCONFIG.gn:99

### ubsan_default_options
Default [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)
options (before the `UBSAN_OPTIONS` environment variable is read at
runtime).  This can be set as a build argument to affect most "ubsan"
variants in `known_variants` (which see), or overridden in
toolchain_args in one of those variants.  Note that setting this
nonempty may conflict with programs that define their own
`__ubsan_default_options` C function.

**Current value (from the default):** `"print_stacktrace=1:halt_on_error=1"`

From //build/config/sanitizers/BUILD.gn:30

### universal_variants

**Current value (from the default):**
```
[{
  configs = []
  name = "release"
  toolchain_args = {
  is_debug = false
}
}]
```

From //build/config/BUILDCONFIG.gn:673

### universe_package_labels
If you add package labels to this variable, the packages will be included
in the 'universe' package set, which represents all software that is
produced that is to be published to a package repository or to the SDK by
the build. The build system ensures that the universe package set includes
the base and cache package sets, which means you do not need to redundantly
include those labels in this variable.

**Current value for `target_cpu = "arm64"`:** `["//tools/net/dev_finder:host", "//tools/vboot_reference:cgpt_host", "//tools/vboot_reference:futility_host", "//bundles:tools"]`

From //products/core.gni:93

**Overridden from the default:** `[]`

From //BUILD.gn:47

**Current value for `target_cpu = "x64"`:** `["//tools/net/dev_finder:host", "//tools/vboot_reference:cgpt_host", "//tools/vboot_reference:futility_host", "//bundles:tools"]`

From //products/core.gni:93

**Overridden from the default:** `[]`

From //BUILD.gn:47

### unpack_debug_archives
To ensure that everything can be built without debug symbols present we
gate weather or not these are consumed on a build argument. When set,
unpack_debug_archives creates an additional build step that unpacks
debug archives in tar.bzip2 format into the .build-id directory

**Current value (from the default):** `false`

From //build/packages/prebuilt_package.gni:13

### update_kernels
(deprecated) List of kernel images to include in the update (OTA) package.
If no list is provided, all built kernels are included. The names in the
list are strings that must match the filename to be included in the update
package.

**Current value (from the default):** `[]`

From //build/images/args.gni:41

### use_cast_runner_canary
If true then the most recent canary version of the Cast Runner is used,
otherwise the most recently validated version is used.

**Current value (from the default):** `false`

From //src/cast/BUILD.gn:11

### use_ccache
Set to true to enable compiling with ccache

**Current value (from the default):** `false`

From //build/toolchain/ccache.gni:9

### use_chromium_canary
Set to use the most recent canary version of prebuilt Chromium components
otherwise the most recently validated version is used.

**Current value (from the default):** `false`

From //src/chromium/BUILD.gn:13

### use_dns_resolver
Transitional flag to enable dns-resolver as the provider for
fuchsia.net.name.LookupAdmin and fuchsia.net.NameLookup instead of netstack.

**Current value (from the default):** `false`

From //src/connectivity/network/BUILD.gn:10

### use_goma
Set to true to enable distributed compilation using Goma.

**Current value (from the default):** `false`

From //build/toolchain/goma.gni:11

### use_lto
Use link time optimization (LTO).

**Current value (from the default):** `false`

From //build/config/lto/config.gni:7

### use_netstack3

**Current value (from the default):** `false`

From //src/connectivity/network/BUILD.gn:6

### use_prebuilt_dart_sdk
Whether to use the prebuilt Dart SDK for everything.
When setting this to false, the preubilt Dart SDK will not be used in
situations where the version of the SDK matters, but may still be used as an
optimization where the version does not matter.

**Current value (from the default):** `true`

From //build/dart/dart.gni:15

### use_prebuilt_ffmpeg
Use a prebuilt ffmpeg binary rather than building it locally.  See
//src/media/lib/ffmpeg/README.md for details.  This is ignored when
building in variant builds for which there is no prebuilt.  In that
case, ffmpeg is always built from source so as to be built with the
selected variant's config.  When this is false (either explicitly or in
a variant build) then //third_party/ffmpeg must be in the source tree,
which requires:
`jiri import -name integration third_party/ffmpeg https://fuchsia.googlesource.com/integration`

**Current value (from the default):** `true`

From //src/media/lib/ffmpeg/BUILD.gn:14

### use_scudo
TODO(davemoore): Remove this entire mechanism once standalone scudo is the
default (DNO-442)
Enable the [Scudo](https://llvm.org/docs/ScudoHardenedAllocator.html)
memory allocator.

**Current value (from the default):** `false`

From //build/config/scudo/scudo.gni:10

### use_thinlto
Use ThinLTO variant of LTO if use_lto = true.

**Current value (from the default):** `true`

From //build/config/lto/config.gni:10

### use_vbmeta
If true, then a vbmeta image will be generated for provided ZBI
and the paving script will pave vbmeta images to the target device.

**Current value (from the default):** `false`

From //build/images/vbmeta.gni:10

### use_vboot
Use vboot images

**Current value (from the default):** `false`

From //build/images/args.gni:10

### using_fuchsia_sdk
Only set in buildroots where targets configure themselves for use with the
Fuchsia SDK

**Current value (from the default):** `false`

From //build/fuchsia/sdk.gni:8

### variants
List of "selectors" to request variant builds of certain targets.  Each
selector specifies matching criteria and a chosen variant.  The first
selector in the list to match a given target determines which variant is
used for that target.

The $default_variants list is appended to the list set here.  So if no
selector set in $variants matches (e.g. if the list is empty, as is the
default), then the first match in $default_variants chooses the variant.

Each selector is either a string or a scope.  A selector that's a string
is a shorthand that gets expanded to a full selector (a scope); the full
selector form is described below.

If a string selector contains a slash, then it's "shorthand/filename".
This is like the plain "shorthand" selector, but further constrained to
apply only to a binary whose `output_name` exactly matches "filename".

The "shorthand" string (a whole string selector or the part before slash)
is first looked up in $variant_shorthands, which see.  If it doesn't match
a name defined there, then it must be the name of a variant.  In that case,
it's equivalent to `{ variant = "..." host = false }`, meaning it applies
to every binary not built to be a host tool.

A full selector is a scope with the following fields.  All the fields
other than `.variant` are matching criteria.  A selector matches if all of
its matching criteria match.  Hence, a selector with no criteria defined
always matches and is referred to as a "catch-all".  The $default_variants
list ends with a catch-all, so each target always chooses some variant.

Selector scope parameters

  * variant
    - Required: The variant to use when this selector matches.  If this
    is a string then it must match a fully-defined variant elsewhere in
    the list (or in $default_variants + $standard_variants, which is
    appended implicitly to the $variants list).  If it's a scope then
    it defines a new variant (see details below).
    - Type: string or scope, described below

  * cpu
    - Optional: If nonempty, match only when $current_cpu is one in the
    - list.
    - Type: list(string)

  * os
    - Optional: If nonempty, match only when $current_os is one in the
    - list.
    - Type: list(string)

  * host
    - Optional: If present, match only in host environments if true or
    non-host environments if false.  This means a context in which
    $is_host is true, not specifically the build host.  For example, it
    would be true when cross-compiling host tools for an SDK build but
    would be false when compiling code for a hypervisor guest system
    that happens to be the same CPU and OS as the build host.
    - Type: bool

  * kernel
    - Optional: If present, match only in kernel environments if true or
    non-kernel environments if false.  This means a context in which
    $is_kernel is true, not just the "kernel" environment itself.
    For different machine architectures there may be multiple different
    specialized environments that set $is_kernel, e.g. for boot loaders
    and for special circumstances used within the kernel.  See also the
    $tags field in $variant, described below.
    - Type: bool

  * environment
    - Optional: If nonempty, a list of environment names that match.  This
    looks at ${toolchain.environment}, which is the simple name (no
    directories) in an environment label defined by environment().  Each
    element can match either the whole environment name, or just the
    "base" environment, which is the part of the name before a `.` if it
    has one.  For example, "host" would match both "host" and "host.fuzz".
    - Type: list(string)

  * target_type
    - Optional: If nonempty, a list of target types to match.  This is
    one of "executable", "host_tool", "loadable_module", "driver", or
    "test".
    Note, test_driver() matches as "driver".
    - Type: list(string)

  * label
    - Optional: If nonempty, match only when the canonicalized target label
    (as returned by `get_label_info(..., "label_no_toolchain")`) is one in
    the list.
    - Type: list(label_no_toolchain)

  * dir
    - Optional: If nonempty, match only when the directory part of the
    target label (as returned by `get_label_info(..., "dir")`) is one in
    the list.
    - Type: list(label_no_toolchain)

  * name
    - Optional: If nonempty, match only when the name part of the target
    label (as returned by `get_label_info(..., "name")`) is one in the
    list.
    - Type: list(label_no_toolchain)

  * output_name
    - Optional: If nonempty, match only when the `output_name` of the
    target is one in the list.  Note `output_name` defaults to
    `target_name`, and does not include prefixes or suffixes like ".so"
    or ".exe".
    - Type: list(string)

An element with a scope for `.variant` defines a new variant.  Each
variant name used in a selector must be defined exactly once.  Other
selectors can refer to the same variant by using the name string in the
`.variant` field.  Definitions in $variants take precedence over the same
name defined in $standard_variants, but it would probably cause confusion
to use the name of a standard variant with a non-standard definition.

Variant scope parameters

  * name
    - Required: Name for the variant.  This must be unique among all
    variants used with the same environment.  It becomes part of the GN
    toolchain names defined for the environment, which in turn forms part
    of directory names used in $root_build_dir; so it must meet Ninja's
    constraints on file names (sticking to `[a-z0-9_-]` is a good idea).

  * globals
    - Optional: Variables in this scope are introduced as globals visible
    to all GN code in the toolchain.  For example, the standard "gcc"
    variant sets `is_gcc = true` in $globals.  This should be used
    sparingly and is safest when restricted to variables that
    $zx/public/gn/BUILDCONFIG.gn sets defaults for.
    - Type: scope

  * toolchain_args
    - Optional: See toolchain().  Variables in this scope must match GN
    build arguments defined somewhere in the build with declare_args().
    Use this when the variant should change something that otherwise is a
    manual tuning variable to set via `gn args`.  *Do not* define
    variables in declare_args() just for the purpose of setting them here,
    i.e. if they should not *also* be available to set via `gn args` to
    affect other variants that don't override them here.  Instead, use
    either $globals (above) or $toolchain_vars (below).
    - Type: scope

  * toolchain_vars
    - Optional: Variables in this scope are visible in the scope-typed
    $toolchain global variable seen in toolchains for this variant.
    Use this to pass along interesting information without cluttering
    the global scope via $globals.
    - Type: scope

  * configs
    - Optional: List of changes to the pre-set $configs variable in targets
    being defined in toolchains for this variant.  This is the same as in
    the $configs parameter to environment().  Each element is either a
    string or a scope.  A string element is simply appended to the default
    $configs list: it's equivalent to a scope element of `{add=["..."]}`.
    The string is the GN label (without toolchain) for a config() target.
    A scope element can be more selective, as described below.
    - Type: list(label_no_toolchain or scope)
      * shlib
        - Optional: If present, this element applies only when
        `current_toolchain == toolchain.shlib` (if true) or
        `current_toolchain != toolchain.shlib` (if false).  That is, it
        applies only in (not ni) the companion toolchain used to compile
        shared_library() and loadable_module() (including driver()) code.
        - Type: bool

      * types
        - Optional: If present, this element applies only to a target whose
        type is one in this list (same as `target_type` in a selector,
        described above).
        - Type: list(string)

      * add
        - Optional: List of labels to append to $configs.
        - Type: list(label_no_toolchain)

      * remove
        - Optional: List of labels to remove from $configs.  This does
        exactly `configs -= remove` so it has the normal GN semantics that
        it's an error if any element in the $remove list is not present in
        the $configs list beforehand.
        - Type: list(label_no_toolchain)

  * implicit_deps
    - Optional: List of changes to the list added to $deps of all linking
    targets in toolchains for this variant.  This is the same as in the
    $implicit_deps parameter to environment().
    - Type: See $configs

  * tags
    - Optional: List of tags that describe this variant.  This list will be
    visible within the variant's toolchains as ${toolchain.tags}.  Its main
    purpose is to match the $exclude_variant_tags list in an environment()
    definition.  For example, several of the standard variants listed in
    $standard_variants use the "useronly" tag.  The environment() defining
    the kernel toolchains uses `exclude_variant_tags = [ "useronly" ]`.
    Then $variants selectors that choose variants that are incompatible
    with the kernel are automatically ignored in the kernel toolchains,
    so there's no need to add `kernel = false` to every such selector.
    - Type: list(string)

  * bases
    - Optional: A list of other variant names that this one inherits from.
    This is a very primitive mechanism for deriving a new variant from an
    existing variant.  All of fields from all the bases except for `name`
    and `bases` are combined with the fields defined explicitly for the
    new variant.  The fields of list type are just concatenated in order
    (each $bases variant in the order listed, then this variant).  The
    fields of scope type are merged in the same order, with a variant
    later in the list overriding values set earlier (so this variant's
    values override all the bases).  There is *only one* level of
    inheritance: a base variant listed in $bases cannot have $bases itself.
    - Type: list(string)


**Current value (from the default):** `[]`

From //zircon/public/gn/toolchain/variants.gni:222

### vbmeta_a_partition

**Current value (from the default):** `""`

From //build/images/args.gni:86

### vbmeta_b_partition

**Current value (from the default):** `""`

From //build/images/args.gni:87

### vbmeta_r_partition

**Current value (from the default):** `""`

From //build/images/args.gni:88

### vendor_linting
Whether libraries under //vendor should be linted.

**Current value (from the default):** `false`

From //build/fidl/fidl_library.gni:13

### virtmagma_debug
Enable verbose logging in virtmagma-related code

**Current value (from the default):** `false`

From //src/graphics/lib/magma/include/virtio/virtmagma_debug.gni:7

### vulkan_sdk

**Current value (from the default):** `""`

From //src/graphics/examples/vkprimer/BUILD.gn:46

### warn_on_sdk_changes
Whether to only warn when an SDK has been modified.
If false, any unacknowledged SDK change will cause a build failure.

**Current value (from the default):** `false`

From //build/sdk/config.gni:11

### weave_build_legacy_wdm
Tells openweave to support legacy WDM mode.

**Current value (from the default):** `false`

From [//third_party/openweave-core/config.gni:29](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#29)

### weave_build_warm
Tells openweave to build WARM libraries.

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:26](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#26)

### weave_system_config_use_sockets
Tells openweave components to use bsd-like sockets.

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:7](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#7)

### weave_with_nlfaultinjection
Tells openweave components to support fault injection.

**Current value (from the default):** `false`

From [//third_party/openweave-core/config.gni:20](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#20)

### weave_with_verhoeff
Tells openweave to support Verhoeff checksum.

**Current value (from the default):** `true`

From [//third_party/openweave-core/config.gni:23](https://fuchsia.googlesource.com/third_party/openweave-core/+/8bde75a87ade87a1fb4afdf5103759b216d56a9e/config.gni#23)

### wlancfg_config_type
Selects the wlan configuration type to use. Choices:
  "client" - client mode
  "ap" - access point mode
  "" (empty string) - no configuration

**Current value (from the default):** `"client"`

From //src/connectivity/wlan/wlancfg/BUILD.gn:16

### zbi_compression
Compression setting for ZBI "storage" items.
This can be either "lz4f" or "zstd", optionally followed by ".LEVEL"
where `LEVEL` can be an integer or "max".

**Current value (from the default):** `"zstd"`

From //build/config/fuchsia/zbi.gni:11

### zedboot_cmdline_args
List of kernel command line arguments to bake into the Zedboot image.
See //docs/reference/kernel_cmdline.md and
[`zedboot_devmgr_config`](#zedboot_devmgr_config).

**Current value (from the default):** `[]`

From //build/images/zedboot/zedboot_args.gni:9

### zedboot_cmdline_files
Files containing additional kernel command line arguments to bake into
the Zedboot image.  The contents of these files (in order) come after any
arguments directly in [`zedboot_cmdline_args`](#zedboot_cmdline_args).
These can be GN `//` source pathnames or absolute system pathnames.

**Current value (from the default):** `[]`

From //build/images/zedboot/zedboot_args.gni:15

### zedboot_devmgr_config
List of arguments to populate /boot/config/devmgr in the Zedboot image.

**Current value (from the default):** `[]`

From //build/images/zedboot/zedboot_args.gni:18

### zedboot_ta_uuids
UUID of TAs to include in the Zedboot build.

**Current value (from the default):** `[]`

From //build/images/ta.gni:13

### zircon_a_partition
arguments to fx flash script

**Current value (from the default):** `""`

From //build/images/args.gni:83

### zircon_args
[Zircon GN build arguments](/docs/gen/zircon_build_arguments.md).
The default passes through GOMA/ccache settings and
[`select_variant`](#select_variant) shorthand selectors.
**Only set this if you want to wipe out all the defaults that
propagate from Fuchsia GN to Zircon GN.**  The default value
folds in [`zircon_extra_args`](#zircon_extra_args), so usually
it's better to just set `zircon_extra_args` and leave `zircon_args` alone.
Any individual Zircon build argument set in `zircon_extra_args` will
silently clobber the default value shown here.

**Current value (from the default):**
```
{
  default_deps = ["//:legacy-arm64", "//:legacy_host_targets-linux-x64", "//:legacy_unification-arm64", "//tools:all-hosts"]
  disable_kernel_pci = false
  goma_dir = "/b/s/w/ir/k/prebuilt/third_party/goma/linux-x64"
  use_ccache = false
  use_goma = false
  variants = []
  zbi_compression = "zstd"
}
```

From //BUILD.gn:97

### zircon_asserts

**Current value (from the default):** `true`

From //build/config/fuchsia/BUILD.gn:218

### zircon_b_partition

**Current value (from the default):** `""`

From //build/images/args.gni:84

### zircon_build_root

**Current value (from the default):** `"//zircon"`

From //src/graphics/lib/magma/gnbuild/magma.gni:18

### zircon_compdb_filter
Compilation database filter. Gets passed to --export-compile-commands=<filter>.

**Current value (from the default):** `"legacy-arm64"`

From //BUILD.gn:71

### zircon_extra_args
[Zircon GN build arguments](/docs/gen/zircon_build_arguments.md).
This is included in the default value of [`zircon_args`](#zircon_args) so
you can set this to add things there without wiping out the defaults.
When you set `zircon_args` directly, then this has no effect at all.
Arguments you set here override any arguments in the default
`zircon_args`.  There is no way to append to a value from the defaults.
Note that for just setting simple (string-only) values in Zircon GN's
[`variants`](/docs/gen/zircon_build_arguments.md#variants), the
default [`zircon_args`](#zircon_args) uses a `variants` value derived from
[`select_variant`](#select_variant) so for simple cases there is no need
to explicitly set Zircon's `variants` here.

**Current value (from the default):** `{ }`

From //BUILD.gn:60

### zircon_extra_deps
Additional Zircon GN labels to include in the Zircon build.

**Current value (from the default):** `[]`

From //BUILD.gn:64

### zircon_r_partition

**Current value (from the default):** `""`

From //build/images/args.gni:85

### zircon_tracelog
Where to emit a tracelog from Zircon's GN run. No trace will be produced if
given the empty string. Path can be source-absolute or system-absolute.

**Current value (from the default):** `""`

From //BUILD.gn:68

### zvb_partition_name
Partition name from where image will be verified

**Current value (from the default):** `"zircon"`

From //build/images/vbmeta.gni:32

### zxcrypt_key_source
This argument specifies from where the system should obtain the zxcrypt
master key to the system data partition.

This value be reified as /boot/config/zxcrypt in both the zircon boot image
and the zedboot boot image, for consumption by fshost and the paver,
respectively.

Acceptable values are:
* "null": the device should use an all-0's master key, as we lack support
for any secure on-device storage.
* "tee": the device is required to have a Trusted Execution Environment
(TEE) which includes the "keysafe" Trusted Application (associated with the
KMS service).  The zxcrypt master key should be derived from a per-device
key accessible only to trusted apps running in the TEE.
* "tee-opportunistic": the device will attempt to use keys from the TEE if
available, but will fall back to using the null key if the key from the TEE
does not work, or if the TEE is not functional on this device.
* "tee-transitional": the device will require the use of a key from the TEE
for new volume creation, but will continue to try both a TEE-sourced key and
the null key when unsealing volumes.

In the future, we may consider adding support for TPMs, or additional logic
to explicitly support other fallback behavior.

**Current value (from the default):** `"null"`

From //build/images/zxcrypt.gni:29

## `target_cpu = "arm64"`

### amlogic_decoder_firmware_path
Path to the amlogic decoder firmware file. Overrides the default in the build.

**Current value (from the default):** `""`

From //src/media/drivers/amlogic_decoder/BUILD.gn:12

### arm_float_abi
The ARM floating point mode. This is either the string "hard", "soft", or
"softfp". An empty string means to use the default one for the
arm_version.

**Current value (from the default):** `""`

From //build/config/arm.gni:20

### arm_optionally_use_neon
Whether to enable optional NEON code paths.

**Current value (from the default):** `false`

From //build/config/arm.gni:31

### arm_tune
The ARM variant-specific tuning mode. This will be a string like "armv6"
or "cortex-a15". An empty string means to use the default for the
arm_version.

**Current value (from the default):** `""`

From //build/config/arm.gni:25

### arm_use_neon
Whether to use the neon FPU instruction set or not.

**Current value (from the default):** `true`

From //build/config/arm.gni:28

### arm_version

**Current value (from the default):** `8`

From //build/config/arm.gni:12

## `target_cpu = "x64"`

### acpica_debug_output
Enable debug output in the ACPI library (used by the ACPI bus driver).

**Current value (from the default):** `false`

From [//third_party/acpica/BUILD.gn:9](https://fuchsia.googlesource.com/third_party/acpica/+/0194bb9d7222c3b5c30573763d2043a62e11838c/BUILD.gn#9)

