# Running tests as components

Tests on Fuchsia can either be run as standalone executables or as components.
Standalone executables are invoked in whatever environment the test runner
happens to be in, whereas components executed in a test runner are run in a
hermetic environment.

These hermetic environments are fully separated from the host services, and the
test manifests can stipulate that new instances of services should be started in
this environment, or services from the host should be plumbed in to the test
environment.

This document aims to outline the idiomatic way for a developer to configure
their test artifacts to be run as components. This document is targeted towards
developers working inside of `fuchsia.git`, and the workflow described is
unlikely to work for SDK consumers.

An example setup of a test component is available at
`//examples/hello_world/rust`.

## Building the test

The exact GN invocations that should be used to produce a test vary between
different classes of tests and different languages. The rest of this document
assumes that test logic is being built somewhere, and that the test output is
something that can be run as a component. For C++ and Rust, this would be the
executable file the build produces.

Further documentation for building tests is [available for Rust][rust_testing].

## Packaging and component-ifying the tests

Once the build rule for building a test executable exists, a component manifest
referencing the executable and a package build rule containing the executable
and manifest must be created.

### Component manifests

The component manifest exists to inform the component framework how to run
something. In this case, it's explaining how to run the test binary. This file
typically lives in a `meta` directory next to the `BUILD.gn` file, and will be
included in the package under a top level directory also called `meta`.

The simplest possible component manifest for running a test would look like
this:

```cmx
{
    "program": {
        "binary": "test/hello_world_rust_bin_test"
    }
}
```

This component, when run, would invoke the `test/hello_world_rust_bin_test`
binary in the package.

This example manifest may be insufficient for many use cases as the program will
have a rather limited set of capabilities, for example there will be no mutable
storage available and no services it can access. The `sandbox` portion of the
manifest can be used to expand on this. As an alternative to the prior example,
this example will give the component access to storage at `/cache` and will
allow it to talk to the service located at `/svc/fuchsia.logger.LogSink`.

```cmx
{
    "program": {
        "binary": "test/hello_world_rust_bin_test"
    },
    "sandbox": {
        "features": [ "isolated-cache-storage" ],
        "services": [ "fuchsia.logger.LogSink" ]
    }
}
```

Test components can also have new instances of services created inside their
test environment, thus isolating the impact of the test from the host. In the
following example, the service available at `/svc/fuchsia.example.Service` will
be handled by a brand new instance of the service referenced by the URL.

```cmx
{
    "program": {
        "binary": "test/hello_world_rust_bin_test"
    },
    "facets": {
        "fuchsia.test": {
            "injected-services": {
                "fuchsia.example.Service": "fuchsia-pkg://fuchsia.com/example#meta/example_service.cmx"
            }
        }
    },
    "sandbox": {
        "services": [
            "fuchsia.example.Service"
        ]
    }
}
```

For a more thorough description of what is valid in a component manifest, please
see the [documentation on component manifests][component_manifest].

### Component and package build rules

With a component manifest written the GN build rule can now be added to create a
package that holds the test component.

```GN
import("//build/test/test_package.gni")

test_package("hello_world_rust_tests") {
  deps = [
    ":bin",
  ]
  tests = [
    {
      name = "hello_world_rust_bin_test"
    }
  ]
}
```

This example will produce a new package named `hello_world_rust_tests` that
contains the artifacts necessary to run a test component. This example requires
that the `:bin` target produce a test binary named `hello_world_rust_bin_test`.

The `test_package` template requires that `meta/${TEST_NAME}.cmx` exist and that
the destination of the test binary match the target name. In this example, this
means that `meta/hello_world_rust_bin_test.cmx` must exist. This template
produces a package in the same way that the `package` template does, but it has
extra checks in place to ensure that the test is set up properly. For more
information, please  see the [documentation on `test_package`][test_package].

## Running tests

Note: If you encounter any bugs or use cases not supported by `fx test`, file a
bug with `fx`.

To test the package, use the `fx test` command with the name of the package:

<pre class="prettyprint">
<code class="devsite-terminal">fx test ${<var>TEST_NAME</var>}</code>
</pre>

If the package you specified is a test component, the command makes your Fuchsia
device load and run said component. However, if the package you specified is a
host test, the command directly invokes that test binary. Note that this can lead
to the execution of multiple components.

### Customize `fx test` invocations

In most cases, you should run the entire subset of test that verify the code
that you are editing. You can run `fx test` with arguments to run specific tests
or test suites, and flags to filter down to just host or device tests. To
customize `fx test`:

<pre class="prettyprint">
<code class="devsite-terminal">fx test [<var>FLAGS</var>] [${<var>TEST_NAME</var>} [${<var>TEST_NAME</var>} [...]]]</code>
</pre>

### Multiple ways to specify a test

`fx test` supports multiple ways to reference a specific test.

- Full or partial paths:

    Provide a partial path to match against all test binaries in children
    directories.

    <pre class="prettyprint">
    <code class="devsite-terminal">fx test //host_x64/gen/sdk</code>
    </pre>


    Provide a full path to match against that exact binary.

    <pre class="prettyprint">
    <code class="devsite-terminal">fx test //host_x64/pm_cmd_pm_genkey_test</code>
    </pre>

    Note: `//` stands for the root of a Fuchsia tree checkout.

- Full or partial [Fuchsia Package URLs][fuchsia_package_url]:

    Provide a partial URL to match against all test components whose Package
    URLs start with the supplied value.

    <pre class="prettyprint">
    <code class="devsite-terminal">fx test <var>fuchsia-pkg://fuchsia.com/my_test_pkg</var></code>
    </pre>

    Provide a full URL to match against that exact test component.

    <pre class="prettyprint">
    <code class="devsite-terminal">fx test <var>fuchsia-pkg://fuchsia.com/my_test_pkg#meta/my_test.cmx</var></code>
    </pre>


- Package name:

    Provide a
    [package name](/docs/concepts/storage/package_url.md#package-name) to
    run all test components in that package:

    <pre class="prettyprint">
    <code class="devsite-terminal">fx test <var>my_test_pkg</var></code>
    </pre>

- Component name:

    Provide a
    [resource-path](/docs/concepts/storage/package_url.md#resource-paths) to
    test a single component in a package:

    <pre class="prettyprint">
    <code class="devsite-terminal">fx test <var>my_test</var></code>
    </pre>

### Running multiple tests

If you want to run multiple sets of Fuchsia tests, configure your Fuchsia build
to include several of the primary testing bundles, build Fuchsia, and then run
all tests in the build:

```bash
fx set core.x64 --with //bundles:tools,//bundles:tests,//garnet/packages/tests:all
fx build
fx test
```

You can also provide multiple targets in a single invocation:

<pre class="prettyprint">
<code class="devsite-terminal">fx test <var>package_1 package_2 component_1 component_2</var></code>
</pre>



### Converting from `run-test` or `run-host-tests`

Note: Please file a bug with `fx` if you find any test invocations that cannot
be converted.

#### `run-test`

For `run-test`, you should always be able to change `fx run-test` to `fx test`,
for example:

<pre class="prettyprint">
<code class="devsite-terminal">fx run-test ${<var>TEST_PACKAGE_NAME</var>}</code>
</pre>

Now becomes:

<pre class="prettyprint">
<code class="devsite-terminal">fx test ${<var>TEST_PACKAGE_NAME</var>}</code>
</pre>


#### `run-host-tests`

For `run-host-tests`, you should always be able to change `fx run-host-tests` to
`fx test`, for example:

<pre class="prettyprint">
<code class="devsite-terminal">fx run-host-tests ${<var>PATH_TO_HOST_TEST</var>}</code>
</pre>

Now becomes:

<pre class="prettyprint">
<code class="devsite-terminal">fx test ${<var>PATH_TO_HOST_TEST</var>}</code>
</pre>


#### the `-t` flag

Unlike `fx run-test` (which operated on *packages*), `fx test` matches against tests
in many different ways. This means that you can easily target tests either by their
package name or directly by a component's name. One common workflow with `run-test`
was to use the `-t` flag to specify a single component:

<pre class="prettyprint">
<code class="devsite-terminal">fx run-test ${<var>PACKAGE_NAME</var>} -t ${<var>NESTED_COMPONENT_NAME</var>}</code>
</pre>

Now, with `fx test`, that simply becomes:

<pre class="prettyprint">
<code class="devsite-terminal">fx test ${<var>NESTED_COMPONENT_NAME</var>}</code>
</pre>


## Running tests (Legacy)

Tests can be exercised with the `fx run-test` command by providing the name of
the package containing the tests.

<pre class="prettyprint">
<code class="devsite-terminal">fx run-test ${<var>TEST_PACKAGE_NAME</var>}</code>
</pre>

This command will rebuild any modified files, push the named package to the
device, and run it.

Tests can also be run directly from the shell on a Fuchsia device with the
`run-test-component` command, which can take either a fuchsia-pkg URL or a
prefix to search pkgfs for.

If using a fuchsia-pkg URL the test will be automatically updated on the device,
but not rebuilt like if `fx run-test` was used. The test will be neither rebuilt
nor updated if a prefix is provided.

In light of the above facts, the recommended way to run tests from a Fuchsia
shell is:

<pre class="prettyprint">
<code class="devsite-terminal">run-test-component `locate ${<var>TEST_PACKAGE_NAME</var>}`</code>
</pre>

The `locate` tool will search for and return fuchsia-pkg URLs based on a given
search query. If there are multiple matches for the query the above command will
fail, so `locate` should be invoked directly to discover the URL that should be
provided to `run-test-component`

[component_manifest]: /docs/concepts/storage/component_manifest.md
[rust_testing]: ../languages/rust/testing.md
[test_package]: test_component.md
[fuchsia_package_url]: /docs/concepts/storage/package_url.md
