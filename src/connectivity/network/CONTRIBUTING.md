# Fuchsia Networking Contributor Guide

Fuchsia Networking welcomes contributions from all. This document defines
contribution guidelines where they differ from or refine on the guidelines that
apply to Fuchsia as a whole.

## Getting Started

Consult the [getting started document][getting_started] to set up your
development environment.

## Contributor Workflow

Consult the [contribute changes document][contribute_changes] for general
contribution guidance and project-wide best practices. The remainder of this
document describes best practices specific to Fuchsia Networking.

## Coding Guidelines

### General Advice

Avoid duplicating code whenever possible. In cases where existing code is not
exposed in a manner suitable to your needs, prefer to extract the necessary
parts into a common dependency.

Avoid unhandled errors and APIs which inherently disallow proper error handling;
for a common example, consider [`fuchsia_async::executor::spawn`][spawn].
`spawn` inherently precludes error passing (since the flow of execution is
severed). In most cases `spawn` can be replaced with a future that is later
included in a [`select`][select] expression ([example commit][spawn_select]) or
simply `await`ed on directly ([example commit][spawn_await]).

Prefer type safety over runtime invariant checking. In other words, arrange your
abstractions such that they cannot express invalid conditions rather than
relying on assertions at runtime.

Avoid [magic numbers][magic_number].

### Comments

When writing comments, take a moment to consider the future reader of your
comment. Ensure that your comments are complete sentences with proper grammar
and punctuation. Note that adding more comments or more verbose comments is not
always better; for example, avoid comments that repeat the code they're anchored
on.

Documentation comments should be self-contained; in other words, do not assume
that the reader is aware of documentation in adjacent files or on adjacent
structures. Avoid documentation comments on types which describe _instances_ of
the type; for example, `AddressSet is a set of client addresses.` is a comment
that describes a field of type `AddressSet`, but the type may be used to hold
any kind of `Address`, not just a client's.

Phrase your comments to avoid references that might become stale; for example:
do not mention a variable or type by name when possible (certain doc comments
are necessary exceptions). Also avoid references to past or future versions of
or past or future work surrounding the item being documented; explain things
from first principles rather than making external references (including past
revisions).

When writing TODOs:
1. Include an issue reference using the format `TODO(1245):`
1. Phrase the text as an action that is to be taken; it should be possible for
   another contributor to pick up the TODO without consulting any external
   sources, including the referenced issue.

### Error messages

As with code comments, consider the future reader of the error messages emitted
by your code. Ensure that your error messages are actionable. For example, avoid
test failure messages such as "unexpected value" - always include the unexpected
value; another example is "expected <variable> to be empty, was non-empty" -
this message would be much more useful if it included the unexpected elements.

Always consider: what will the reader do with this message?

### Source Control Best Practices

Commits should be arranged for ease of reading; that is, incidental changes
such as code movement or formatting changes should be committed separately from
actual code changes.

Commits should always be focused. For example, a commit could add a feature,
fix a bug, or refactor code, but not a mixture.

Commits should be thoughtfully sized; avoid overly large or complex commits
which can be logically separated, but also avoid overly separated commits that
require code reviews to load multiple commits into their mental working memory
in order to properly understand how the various pieces fit together. **If your
changes require multiple commits, consider whether those changes warrant a
design doc or [RFC][rfc_process]**.

#### Commit Messages

Commit messages should be _concise_ but self-contained (avoid relying on bug
references as explanations for changes) and written such that they are helpful
to people reading in the future (include rationale and any necessary context).

Avoid superfluous details or narrative.

Commit messages should consist of a brief subject line and a separate
explanatory paragraph in accordance with the following:
1. [Separate subject from body with a blank line](https://chris.beams.io/posts/git-commit/#separate)
1. [Limit the subject line to 50 characters](https://chris.beams.io/posts/git-commit/#limit-50)
1. [Capitalize the subject line](https://chris.beams.io/posts/git-commit/#capitalize)
1. [Do not end the subject line with a period](https://chris.beams.io/posts/git-commit/#end)
1. [Use the imperative mood in the subject line](https://chris.beams.io/posts/git-commit/#imperative)
1. [Wrap the body at 72 characters](https://chris.beams.io/posts/git-commit/#wrap-72)
1. [Use the body to explain what and why vs. how](https://chris.beams.io/posts/git-commit/#why-not-how)

The body may be omitted if the subject is self-explanatory; e.g. when fixing a
typo. The git book contains a [Commit Guidelines][commit_guidelines] section
with much of the same advice, and the list above is part of a [blog
post](https://chris.beams.io/posts/git-commit/) by [Chris
Beams](https://chris.beams.io/).

Commit messages should make use of issue tracker integration. See [Commit-log
message integration][commit_log-message-integration] in the monorail
documentation.

Commit messages should never contain references to any of:
1. Relative moments in time
1. Non-public URLs (such as go/, fxb/, fxr/, and the like)
1. Individuals
1. Hosted code reviews (such as on fuchsia-review.googlesource.com)
    + Refer to commits in this repository by their SHA-1 hash
    + Refer to commits in other repositories by public web address (such as
      https://fuchsia.googlesource.com/fuchsia/+/67fec6d)
1. Other entities which may not make sense to arbitrary future readers

### Philosohpy

This section is inspired by [Flutter's style guide][flutter_philosophy], which
contains many general principles that you should apply to all your programming
work. Read it. The below calls out specific aspects that we feel are
particularly important.

#### Be Lazy

Do not implement features you don't need. It is hard to correctly design unused
code. This is closely related to the commit sizing advice given above; adding a
new data structure to be used in some future commit is akin to adding a feature
you don't need - it is exceedingly hard for your code reviewer to determine if
you've designed the structure correctly because they (and you!) can't see how it
is to be used.

#### Go Down the Rabbit Hole

You will occasionally encounter behaviour that surprises you or seems wrong. It
probably is! Invest the time to find the root cause - you will either learn
something, or fix something, and both are worth your time. Do not work around
behaviour you don't understand.

## Tips & Tricks

### `fx set`

Run the following command to build all tests and their dependencies:

```
fx set core.x64 --with //src/connectivity/network:tests
```

If you're working on changes that affect `fdio` and `third_party/go`, add:

```
--with-base //garnet/packages/tests:zircon --with //third_party/go:go_stdlib_tests
```

[getting_started]: /docs/getting_started.md
[contribute_changes]: /docs/development/source_code/contribute_changes.md
[spawn]: https://fuchsia.googlesource.com/fuchsia/+/a874276/src/lib/fuchsia-async/src/executor.rs#30
[select]: https://docs.rs/futures/0.3.4/futures/macro.select.html
[spawn_select]: https://fuchsia.googlesource.com/fuchsia/+/0c00fd3%5E%21/#F3
[spawn_await]: https://fuchsia.googlesource.com/fuchsia/+/038d2b9%5E%21/#F0
[magic_number]: https://en.wikipedia.org/wiki/Magic_number_(programming)
[rfc_process]: /docs/project/rfcs/0001_rfc_process.md
[commit_guidelines]: https://www.git-scm.com/book/en/v2/Distributed-Git-Contributing-to-a-Project#_commit_guidelines
[commit_log-message-integration]: https://chromium.googlesource.com/infra/infra/+/master/appengine/monorail/doc/userguide/power-users.md#commit_log-message-integration
[flutter_philosophy]: https://github.com/flutter/flutter/wiki/Style-guide-for-Flutter-repo#philosophy
