fuchsia::modular::Action Template
=============
> Status: DRAFT
>
> NOTE: There is a proposal to "publish" these not to Fuchsia packages, but
> rather somewhere within the build tree, since they are strictly only necessary
> at build time, not at runtime.

An **action** represents a high-level operation that a `Module` can perform. The
operations are described independently of the specific data types they operate
on or the modules that implement them.

An **action template** defines the *call signature* and *semantics* for a single
*action*. Actions are labels given to high-level operations in Fuchsia that may be
performed by [`Modules`](../module.md) with a given set of data.

Example actions could include `Navigate`, `Preview` or `Summarize`.
> TODO(thatguy): Add more examples, and once we have platform-provided actions,
> include those here.

Modules implement actions and must adhere to the call signature and behavior
described in the action template.

There is no prescribed ontology of actions. Anyone may publish an action template and
begin to utilize the action defined within. <s>For this reason, actions are
unambiguously identified by both the package in which they were published as
well as the action itself.</s>

> TODO(thatguy): Add information about how action names are derived & how they
> are guaranteed to avoid collisions by using author identity as a component.
> 
> TODO(thatguy): Add information about where actions are published.

## File format

fuchsia::modular::Action templates are defined in a <s>`action_template` metadata file</s> JSON file.
The content of the file is a JSON array
of dictionaries. The full JSON schema, for reference, can be [found
here](../src/package_manager/metadata_schemas/action_template.json).

Below is an example, followed by a detailed breakdown of the properties.

## Example

The following `action_template` file defines three actions: `Preview`,
`Navigate` and `Pick`.

`Preview` accepts a single required argument (called a "noun"), which
represents the `fuchsia::modular::Entity` that will be previewed.

`Navigate` accepts two nouns, one of which (the *destination*) is required.

`Pick` accepts an optional source (from which to pick an item), and returns a
single "picked" entity.

```javascript
[
  {
    "name": "com.google.fuchsia.preview.v1",
    "nouns": [
      {
        "name": "entityToPreview",
        "direction": "input",
        "required": true
      }
    ],
    "doc": "docs/preview.md"
    // The above doc file contains:
    // Renders a static or interactive preview of |entityToPreview|.
    // May include an affordance which when selected displays a
    // larger preview, or launches a new Module / experience.
  },
  {
    "name": "com.google.fuchsia.navigate.v1",
    "nouns": [
      {
        "name": "start",
        "direction": "input"
      },
      {
        "name": "destination",
        "direction": "input",
        "required": true
      }
    ],
    "doc": "docs/navigate.md"
  },
  {
    "name": "com.google.fuchsia.pick.v1",
    "nouns": [
      {
        "name": "source",
        "direction": "input"
      },
      {
        "name": "picked",
        "direction": "output"
      }
    ],
    "doc": "docs/pick.md"
  }
  // TODO: Add an example that uses input/output direction on a noun.
]
```

## Details

Let's go through the properties that make up an action template.

### action name

```javascript
"name": "com.google.fuchsia.preview.v1",
```
> TODO(thatguy): This is a machine-readable name, not a human-readable name. Add
> something for humans that supports localization.

This is the action's name. It uniquely identifies this action.

> TODO(thatguy): Add information about how action names are associated with the author
> so no collisions are possible.

fuchsia::modular::Action names allow `Module` [metadata files](module.md) to reference this action
template when they declare that they provide an implementation, and allows [`Daisies`](../intent.md) to reference the same action when a client requests the associated action to take place.

### nouns

```javascript
"nouns": [
  {
    "name": "entityToPreivew",
    "direction": "input",
    "required": true
  }
],
```

> TODO: There is no way to specify that a noun argument should be a list of
> Entities.  This may be possible simply by adding a `is_list` property.

Nouns give names to individual or sets of [`fuchsia::modular::Entity`](../entity.md) references
passed between `Modules` at runtime.

Each noun declared in an `action` definition must have the following properties:

* `name`: limited to the following characters: `[a-zA-Z0-9_]`
* `direction`: any of `input`, `output` and `input/output`.
  - `input`: only the target `Module`'s parent may set the `fuchsia::modular::Entity`
    reference(s) value for this noun.
  - `output`: only the target `Module` may set the `fuchsia::modular::Entity` reference(s).
  - `input/output`: either may set the `fuchsia::modular::Entity` reference(s).
* `required`: `true` or `false`. Default: `false`

Note that nouns do not specify `fuchsia::modular::Entity` types here. These are
defined at the time of declaring an implementation of this `action`
by a `Module`, which is done in a [`Module` metadata file](module.md).


### JSON parameters

TODO

### return JSON

TODO

### documentation

```javascript
"doc": "docs/preview.md",
```
`doc` is a path to a Markdown file, relative to the root of the package.

The documentation should identify expected behavior for `Modules` that
implement this `action` and what role each `noun` plays in that behavior.
