Module Metadata File
===
> Status: DRAFT

A Module metadata file defines the runtime capabilities of a single `Module`
(**TODO: link**). A Module expresses its capabilities by providing a description
of an implementation for an `action`s (**TODO**: link to action/action template doc).

The file is part of a Fuchsia package (**TODO: link**), is named `module` and
placed within the `meta/` of the package.

The `module` file contents are a JSON-encoded object (JSON schema [available
here](../../../build/module_manifest_schema.json)) that defines a
single action implementation.

## Example `module` metadata file

Following are two sample `module` files. Each describes a `Module` that
implements different `action`s.

```javascript
{
  "binary": "bin/myPersonPreviewer",
  "suggestion_headline": "See details about person",
  "action": "com.google.fuchsia.preview.v1",
  "parameters": [
    {
      "name": "entityToPreview",
      "type": "https://fuchsia.instagram.com/types/Friend"
    }
  ],
  "composition_pattern": "ticker"
}
```
```javascript
{
  "binary": "bin/myContactPicker",
  "suggestion_headline": "Pick an instagram friend",
  "action": "com.google.fuchsia.pick.v1",
  "parameters": [
    {
      "name": "source",
      "type": "https://fuchsia.instagram.com/types/FriendRepository"
    },
    {
      "name": "picked",
      "type": "https://fuchsia.instagram.com/types/Friend"
    }
  ]
}
```

## Detailed description

Let's break down one of the examples above and go through the properties in detail.

### Attributes that define behavior

#### binary

```javascript
"binary": "bin/myPreviewer",
```

Specifies the relative path from the root of the package where the Module
executable file can be found. Different action implementations within the same
package can share a single `binary`.

#### suggestion_headline

> TODO(thatguy): If we keep this, expand it to support 
> templating based on the entity args.

```javascript
"suggestion_headline": "See details about person",
```

The `suggestion_headline` attribute provides human-readable
text. It will be used if this `Module` is suggested for inclusion
in the current Story.

#### Action

```javascript
"action": "com.google.fuchsia.preview.v1",
```
> NOTE: The exactly format of the action attribute is likely to evolve.

The `action` attribute identifies which action this Module implements. This dictates the semantic function of this Module, as well as the role of each parameter.

The `action` must match an `action` name in an associated
[`meta/action_template`](action_template.md) file.

> TODO(thatguy): Add information about where actions are discoverable.

#### Parameters

```javascript
"parameters": [
  {
    "name": "entityToPreview",
    "type": "https://fuchsia.instagram.com/types/Friend"
  },
  ...
],
```

In the [action template](action_template.md), the parameters are given names but not
assigned concrete Entity types (**TODO** link). Here, we constrain this
action implementation to operate on a specific set of [Entity](../entity.md)
types.

Each parameter in the action template gets an entry in `parameter_constraints`. Each entry
is made up of the following fields:

* `name`: this is the name of the parameter given in the [action template](action_template.md)
* `type`: an [Entity](../entity.md) types.

   > TODO(thatguy): Add information about where entity types & schemas are discoverable.

   > TODO(thatguy): The semantics of `type` doesn't make a lot of sense for *output*
     or maybe *input/output* parameters.

At runtime, this `Module` will communicate with its parent (the invoker of the
`Module`) through multiple [`Link`](../../services/story/link.fidl) interfaces. The `Link` enforces the
typing described here, making any attempt to write an `Entity` with an
incompatible type an error.

#### composition_pattern

```javascript
"composition_pattern": "ticker",
```
Specifies the composition pattern with which the module will shown with existing module(s) in
the story. For example, the ticker pattern described above gives a signal to the story shell that
the module should be placed below another module that it may share a link with.

Currently supported patterns:
  ticker: Shown at the bottom of the screen underneath another module.
  comments-right: Shown to the right of another module.

> NOTE: the set of the patterns supported and their names will likely evolve with time.

#### outgoing services

TODO

### Attributes that define display constraints

TODO
