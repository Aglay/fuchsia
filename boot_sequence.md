Fuchsia Boot Sequence
=====================

This document describes the boot sequence for Fuchsia from the time the Magenta
layer hands control over to the Fuchsia layer.  This document is a work in
progress that will need to be extended as we bring up more of the system

# Layer 0: [init](https://fuchsia.googlesource.com/init)

`init`'s job is to be a minimal entry point into the Fuchsia layer.  In the
current system, we could collapse it with `application_manager`, but we’re going
to keep it for a while to see whether it is useful to have as a shim, for
example to support update or re-int related use cases.

`devmgr` starts `init` with `MX_HND_TYPE_APPLICATION_LAUNCHER` populated with an
`mx::channel` that the `devmgr` will use to communicate with the
`application_manager`.  At the moment, this channel serves several development
use cases (e.g., the `@` command in the shell).  As we straighten out the
system, we might find that we no longer have need for this channel.

`init` creates a job to hold the Fuchsia user space processes, starts
`application_manager`, and transfers to `application_manager` the handle `init`
receives in `MX_HND_TYPE_APPLICATION_LAUNCHER`. `init` them waits for
`application_manager` to terminate. If that ever happens, `init` kills the
userspace job and attempts to restart user space.

# Layer 1: [application_manager](https://fuchsia.googlesource.com/application/+/master/src/)

`application_manager`'s job is to host the environment tree and help create
processes in these environments.  Processes created by `application_manager`
have an `mx::channel` back to their environment, which lets them create other
processes in their environment and to create nested environments.

At startup, `application_manager` creates an empty root environment and creates
the initial apps listed in `/system/data/application_manager/initial.config` in
that environment. Typically, these applications create environments nested
directly in the root environment. The default configuration contains one initial
app: `bootstrap`.

# Layer 2: [bootstrap](https://fuchsia.googlesource.com/modular/+/master/src/bootstrap/)

`bootstrap`'s job is to create the boot environment and create a number of
 initial applications in the boot environment.

The services that `bootstrap` offers in the boot environment are not provided by
bootstrap itself. Instead, when bootstrap receives a request for a service for
the first time, `bootstrap` lazily creates the appropriate app to implement that
service and routes the request to that app. The table of which applications
implement which services is contained in the
`/system/data/bootstrap/services.config` file. Subsequent requests for the same
service are routed to the already running app. If the app terminates,
`bootstrap` will start it again the next time it receives a request for a
service implemented by that app.

`bootstrap` also runs a number of application in the boot environment at
startup. The list of applications to run at startup is contained in the
`/system/data/bootstrap/apps.config` file. By default, this list includes
`/boot/bin/sh /system/autorun`, which is a useful development hook for the
boot sequence, and `run-vc`, which creates virtual consoles.

# Layer 3: ... to be continued

(In the future, the boot sequence will likely continue from this point into the
device runner, user runner, user shell, etc.)
