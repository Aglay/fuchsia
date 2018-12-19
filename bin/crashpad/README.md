# Crash reporting

For development, it is often easier to dump the crash information in the logs as
the crash happens on device. For devices in the field, we want to be able to
send a report to a remote crash server as well as we might not have access to
the devices' logs. We use
[Crashpad](https://chromium.googlesource.com/crashpad/crashpad/+/master/README.md)
as the third-party client library to talk to the remote crash server.

## Adding report annotations

We collect various info in addition to the stack trace, e.g., process names,
board names, that we add as annotations to the crash reports. To add a new
annotation, simply add a new field in the map returned by
[::fuchsia::crash::MakeAnnotations()](https://fuchsia.googlesource.com/garnet/+/master/bin/crashpad/report_annotations.h).

## Testing

To test your changes, on a real device, we have some unit tests and two helper
programs to simulate a kernel crash and a C userspace crash.

To run the unit tests:

```sh
(host) $ fx run-test-component crashpad_analyzer_tests
```

As for the helper programs, after running each one of them (see commands in
sections below), you should then look each time for the following line in the
syslog:

```sh
(host)$ fx syslog --tag crash
...
successfully uploaded crash report at $URL...
...
```

Click on the URL (contact frousseau if you don't have access and think you
should) and check that the report matches your expectations, e.g., the new
annotation is set to the expected value.

### Kernel crash

The following command will cause a kernel panic:

```sh
(target)$ k crash
```

The device will then reboot and the system should detect the kernel crash log,
attach it to a crash report and try to upload it. Look at the syslog upon
reboot.

### C userspace crash

The following command will cause a write to address 0x0:

```sh
(target)$ crasher
```

You can immediately look at the syslog.

## Question? Bug? Feature request?

Contact frousseau, or file a
[bug](https://fuchsia.atlassian.net/secure/CreateIssueDetails!init.jspa?pid=11718&issuetype=10006&priority=3&components=11950)
or a
[feature request](https://fuchsia.atlassian.net/secure/CreateIssueDetails!init.jspa?pid=11718&issuetype=10005&priority=3&components=11950).
