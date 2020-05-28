# Run components in a test

This guide demonstrates how to run a test component.

Note: This guide is specific to [components v2](/docs/concepts/components).

A component instance is started in Fuchsia when other components request a
[capability](/docs/concepts/components/capabilities/README.md) from it. For
instance, a test component is started by a test manager, which is
[also a component](/docs/concepts/components/introduction.md#everything_is_a_component_almost),
on behalf of a request to run a test.

The guide uses the
<code>[hello_world_bin_test](/examples/components/basic/meta/hello_world_bin_test.cml)</code>
component in the <code>[basic](/examples/components/basic)</code> example package. When you
run this package’s `hello-world-tests` test suite, the test manager starts the
`hello_world_bin_test` component. As a result, the component’s test binary runs
on a Fuchsia device.

To run this test component, the steps are:

*   [Build a Fuchsia image](#build-a-fuchsia-image).
*   [Start the emulator](#start-the-emulator).
*   [Run the test suite](#run-the-test-suite).

## Prerequisites

Verify the following requirements:

*   [Set up the Fuchsia development environment](/docs/development/source_code/README.md).
*   [Run the Fuchsia emulator](/docs/development/run/femu.md).

## Build a Fuchsia image {#build-a-fuchsia-image}

Configure and build your Fuchsia image to include the test component:

1.  To include a specific component, run the `fx set` command with the `--with`
    option:

    ```posix-terminal
    fx set core.x64 --with //examples/components/basic:hello-world-tests
    ```

    `//examples/components/basic` is the directory of the example package and
    `hello-world-tests` is the name of the build target defined in the package's
    <code>[BUILD.gn](/examples/components/basic/BUILD.gn)</code> file.

1.  Build your Fuchsia image:

    ```posix-terminal
    fx build
    ```

    When the `fx build` command completes, your new Fuchsia image now includes
    the `hello_world_bin_test` component that can be
    [fetched and launched on-demand](/docs/concepts/build_system/boards_and_products.md#universe).

## Start the emulator {#start-the-emulator}

Start the emulator with your Fuchsia image and run a
[package repository server](/docs/development/build/fx.md#serve-a-build):

Note: The steps in this section assume that you don't have any terminals
currently running FEMU or the `fx serve` command.

1.  Configure an IPv6 network for the emulator (you only need to do this once):

    ```posix-terminal
    sudo ip tuntap add dev qemu mode tap user $USER && sudo ifconfig qemu up
    ```

1.  In a new terminal, start the emulator:

    ```posix-terminal
    fx emu -N
    ```

1.  Set the emulator to be your device:

    ```posix-terminal
    fx set-device
    ```

    If you have multiple devices, select `step-atom-yard-juicy` (the emulator’s
    default device name), for example:

    <pre>
    $ fx set-device

    Multiple devices found, please pick one from the list:
    1) rabid-snort-wired-tutu
    2) step-atom-yard-juicy
    #? <b>2</b>
    New default device: step-atom-yard-juicy
    </pre>

1.  In another new terminal, start a package repository server:

    ```posix-terminal
    fx serve
    ```

    Keep the `fx serve` command running as a package server for your device.

## Run the test suite {#run-the-test-suite}

Run the `hello-world-tests` test suite:

```posix-terminal
fx test hello-world-tests
```

This command prints the following output:

```none
$ fx test hello-world-tests

...

[0/1] 00:00 🤔  /home/fuchsia/.jiri_root/bin/fx shell run-test-suite fuchsia-pkg://fuchsia.com/hello-world-tests#meta/hello_world_bin_test.cm
 >> Runtime has exceeded 2 seconds (adjust this value with the -s|--slow flag)
Running test 'fuchsia-pkg://fuchsia.com/hello-world-tests#meta/hello_world_bin_test.cm'

[RUNNING]   tests::assert_0_is_0
[PASSED]    tests::assert_0_is_0
1 out of 1 tests passed...
fuchsia-pkg://fuchsia.com/hello-world-tests#meta/hello_world_bin_test.cm completed with result: PASSED

[1/1] 00:05 ✅  /home/fuchsia/.jiri_root/bin/fx shell run-test-suite fuchsia-pkg://fuchsia.com/hello-world-tests#meta/hello_world_bin_test.cm

🎉  Ran 1 tests with 0 failures (use the -v flag to see each test) 🎉
```

The output shows that the `hello_world_bin_test` component is fetched from the
package repository server and the component instance runs the test binary on the
Fuchsia device (the emulator). See
<code>[hello_world.rs](/examples/components/basic/src/hello_world.rs)</code>
for the source code of this test binary.

