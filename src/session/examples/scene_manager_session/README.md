# Scene Manager Session Example

Reviewed on: 2020-02-04

This directory contains an example implementation of a session that uses a [`SceneManager`](//src/session/lib/scene_management/src/scene_manager.rs), specifically the [`FlatSceneManager`](//src/session/lib/scene_management/src/flat_scene_manager.rs), to handle rendering things to the screen.

## Building `scene_manager_session`

The example sessions are included in the build when you include `//src/session` with your `fx set`:

```
fx set <PRODUCT>.<BOARD> --with-base=//src/session,//src/session/bin/session_manager:session_manager.config
```

To see a list of possible products, run: `fx list-products`.

To see a list of possible boards, run: `fx list-boards`.

## Running `scene_manager_session`
### Boot into `scene_manager_session`

To boot into `scene_manager_session`, first edit the [session manager cml](//src/session/bin/session_manager/meta/session_manager.cml) file to set the input session's component url as the argument:
```
args: [ "-s", "fuchsia-pkg://fuchsia.com/scene_manager_session#meta/scene_manager_session.cm" ],
```
and run
```
fx update
```

To build the relevant components and boot into the session, follow the instructions in [//src/session/README.md](//src/session/README.md).

### Launch `scene_manager_session` from Command Line

To instruct a running `session_manager` to launch the session, run:
```
fx shell session_control -s fuchsia-pkg://fuchsia.com/scene_manager_session#meta/scene_manager_session.cm
```

## Testing

Add `--with //src/session:tests` to your existing `fx set` command to include the `scene_manager_session` unit tests in the build. The resulting `fx set` looks like:
```
fx set <PRODUCT>.<BOARD> --with-base=//src/session,//src/session/bin/session_manager:session_manager.config --with //src/session:tests
```
To see a list of possible products, run: `fx list-products`.

To see a list of possible boards, run: `fx list-boards`.

The tests are available in the `scene_manager_session_tests` package. To run the tests, use:
```
$ fx run-test scene_manager_session_tests
```

## Source Layout

The entry point and session units tests are located in `src/main.rs`.
