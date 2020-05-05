# Fuchsia SDK

This directory contains the source code for the core of the [Fuchsia
SDK](../docs/glossary.md#fuchsia-sdk). The SDK itself is produced as an output
of the build by processing the contents of this directory.  For example, this
directory might contain the source code for a library that is included in the
SDK as a prebuilt shared library.

Software outside of the [Platform Source
Tree](../docs/glossary.md#platform-source-tree) should depend only on the Fuchsia
SDK.

> [Learn more](../docs/concepts/sdk/README.md)

## Categories

Not all the interfaces defined in this directory are part of every Fuchsia SDK.
Instead, interfaces have a `category` label that determines whether the
interface can be included in a given SDK. For example, interfaces with the
`internal` category are available only within the
[Platform Source Tree](../docs/glossary.md#platform-source-tree).
Interfaces with the `partner` category are additionally available to partner
projects. See [sdk_atom.gni](../build/sdk/sdk_atom.gni) for more details.

## Governance

The API surface described by the SDK is governed by the
[Fuchsia API Council](../docs/concepts/api/council.md) and should conform to
the appropriate [API rubrics](../docs/concepts/api/README.md).
