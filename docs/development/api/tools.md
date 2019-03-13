# Developer tool guidelines

This section provides guidelines on *creating* CLI and GUI tools for
Fuchsia.

For information on existing tools, please refer to documentation for those
tools.

## Other topics

- [Command-line tool requirements](cli.md)
    - [CLI --help requirements](cli_help.md)
- GUI Tool requirements (needs writing)

## Packaging a tool with the core SDK

The core SDK will not contain only:

  * The tool binary itself.

  * The [dev_finder](https://fuchsia.googlesource.com/fuchsia/+/refs/heads/master/sdk/docs/device_discovery.md)
    tool which can enumerate Fuchsia devices to get their names.

  * A document in
    [//sdk/docs](https://fuchsia.googlesource.com/fuchsia/+/refs/heads/master/sdk/docs/)
    describing the contract of this tool and how to connect it to the target
    system. The target audience of this document is people writing integration
    scripts rather than being an end-user-friendly “how-to” (debugger example).

## Environment-specific SDKs

The `dev_finder` abstracts device listing and selection across all SDK
variants. With the right tool design, the extent of integration required should
be to run `dev_finder` to get the address and pass the address to the tool with
other environment-specific flags. In the case of the debugger the tool-specific
code would:

  * Connect to a shell (this should be a primitive provided by the
    environment-specific SDK) on the target and run the `debug_agent`.

  * Run zxdb with the address provided by `dev_finder`, passing any local
    settings files and symbol paths on the command-line.

## Tool requirements

Tools should allow all environment parameters to be passed in via command-line
arguments. Examples include the location of settings files and symbol
locations. This allows different SDKs to be hermetic.

Tools should be written to make writing environment-specific scripts as simple
as possible. For example, the debugger should automatically retry connections
(DX-1091) so the current behavior of waiting for the port to be open in the
launch scripts can be removed.

Tool authors are responsible for:

  * Writing the tool with the appropriate interface.
  * Providing documentation on this interface in //sdk/docs.
  * Currently please reach out to get bugs filed on individual SDKs. We are
    working on a better process for this (DX-1066).

