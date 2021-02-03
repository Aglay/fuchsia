# session_manager

Reviewed on: 2021-01-21

`session_manager` is the component that runs and manages sessions. For more information on what sessions are and what roles they play in products built on Fuchsia, see [Session Framework](/docs/concepts/session/introduction.md)

## Building

Add the `session_manager` component to builds by including `--with-base //src/session` in the `fx set` invocation followed by rebuilding and re-paving the device.

Product configurations built on Session Framework (such as `fx set workstation.x64`) include `//src/session` by default.

## Running

`session_manager` is launched in one of two ways: manually or automatically on system boot.

In general, running manually is useful during development, while running on boot is desirable for a production configuration.

### Running manually

Use the [`session_control`](/src/session/tools/session_control/README.md) tool to run a specific session. For example:

```
$ fx shell session_control launch fuchsia-pkg://fuchsia.com/your_session#meta/your_session.cm
```

### Running on boot

In the general case, running `session_manager` on boot is done in order to launch a specific session on boot. In the event you wish to run `session_manager` on boot without *also* launching a specific session, see "Alternative: run on boot without a session" below.

Create a configuration file with the URL to your session as follows:

```
{
  "session_url": "fuchsia-pkg://fuchsia.com/your_package#meta/your_session.cm"
}
```

Add to your `BUILD.gn` file:

```
import("//src/session/build/session_config.gni")

session_config("your_session_config") {
  config = "path/to/config.json"
}
```

And ensure that the target `:your_session_config` is included in the base image (for example, using `--with-base`, or as a direct dependency of a product build group).

Re-build, re-pave, and restart your device and it will boot into `session_manager` and launch your session.

#### Alternative: run on boot without a session

Configure your build to also include the `sysmgr` configuration file `session_manager.config`:

```
$ fx set core.x64 --with-base=//src/session,//src/session/bin/session_manager:session_manager.config
```

Rebuild and re-pave the device and `session_manager` will start automatically.

## Testing

Unit tests for `session_manager` are included in the `session_manager_tests` package, and integration tests are included in the `session_manager_integration_tests` package.

Both can be included in your build by adding `--with //src/session:tests` to your `fx set`.

```
$ fx test session_manager_tests
$ fx test session_manager_integration_tests
```

## Source layout

The entrypoint for `session_manager` is in `src/main.rs` with implementation details in other files within `src/`.

Unit tests are co-located with the code, while integration tests are in [`//src/session/tests`](/src/session/tests).