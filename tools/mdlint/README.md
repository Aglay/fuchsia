# mdlint

`mdlint` is a Markdown linter. It is designed to enforce specific rules about
how Markdown is to be written in the Fuchsia Source Tree. This linter is
designed to parse [Hoedown](https://github.com/hoedown/hoedown) syntax, as used
on the [fuchsia.dev site](http://fuchsia.dev).

## Using mdlint

Configure, and build

    fx set core.x64 --with //tools/mdlint:host # or similar
    fx build

Then run

    fx mdlint --root-dir docs \
              --enable no-extra-space-on-right \
              --enable casing-of-anchors \
              --enable bad-lists \
              --enable verify-internal-links

## Testing

Configure

    fx set core.x64 --with //tools/mdlint:tests # or similar

Then test

    fx test mdlint_tests

## Implementation

The linter parses Markdown files successively, typically all files under a root
directory.

Each Markdown file is read as a stream of UTF-8 characters (runes), which is
then [tokenized](#tokenization) into a stream of token. This stream of token is
then pattern matched and recognized into a stream of events. This layered
processing is similar to how streaming XML parsers are structured, and offers
hook points for [linting rules](#linting-rules) to operate at various levels of
abstraction.

### Tokenization {#tokenization}

Because Markdown attaches important meaning to whitespace characters (e.g.
leading space to form a list element), and certain constructs' meaning depend on
their context (e.g. links, or section headers), the tokenization differs
slightly from what is typically done for more standard programming languages.

Tokenization segments streams of runes into meaningful chunks, named tokens.

All whitespace runes are considered tokens, and are preserved in the token
stream. For instance, the text `Hello, World!` would consist of three tokens: a
text token (`Hello,`), a whitespace token (` `, and lastly followed by a text
token (`World!`).

Certain tokens are classified and tokenized differently depending on their
preceding context. Consider for instance `a sentence (with a parenthesis)` which
is simply text tokens separated by whitespace tokens, as opposed to `a
[sentence](with-a-link)` where instead need to identify both the link
(`sentence`) and it's corresponding URL (`with-a-link`). Other similar examples
are headings, which are denoted by a series of pound runes (`#`) at the start of
a line, or heading anchors `{#like-so}`, which may only appear on a heading
line.
