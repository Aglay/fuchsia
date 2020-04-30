## Build the test

```shell
$ fx set <product>.<arch> --with //src/ui/tests/e2e_input_tests/touch:tests
```

## Run the test

Remember to kill a running Scenic before starting the test.
(If the zircon console is running, you don't need to do this.)

```shell
$ fx shell killall scenic.cmx
```


To run the fully-automated test, use this fx invocation:

```shell
$ fx test touch-input-test
```

## Performance tracking
This test suite uses the [Fuchsia tracing system](https://fuchsia.dev/fuchsia-src/concepts/tracing)
to collect metrics.

TODO(fxb/50245): Add description about how trace metrics are tracked in CQ.

### Add trace metrics to the test
This test suite uses the category `touch-input-test` to log trace events. Any new categories added
to a test will need to be included in the `fx traceutil record` command below.

Trace event types can be found in [`libtrace`](//zircon/system/ulib/trace/include/lib/trace/event.h).

### Record a trace of the test

Add the tracing package to your `fx set`:
```shell
$ fx set <product>.<arch> --with //src/ui/tests/e2e_input_tests/touch:tests --with-base=//garnet/packages/prod:tracing
```

To record a trace of the test, use this fx invocation:
```shell
$ fx traceutil record --duration 20s --categories touch-input-test fuchsia-pkg://fuchsia.com/touch-input-test#meta/touch-input-test.cmx
```

Note: The default duration for `traceutil record` is 10 seconds. When running locally, package
resolving can take more than 10 seconds. If the recording ends before the test completes,
increase the amount of time in the `--duration` flag.

## Play with the flutter client

To play around with the flutter client used in the automated test, invoke the client like this:

```shell
$ present_view fuchsia-pkg://fuchsia.com/one-flutter#meta/one-flutter.cmx
```

