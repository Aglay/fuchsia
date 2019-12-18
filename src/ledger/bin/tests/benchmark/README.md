#benchmark

## Overview
This directory contains Ledger [trace-based benchmarks]. Each benchmark is
implemented as a client connected to Ledger and using its API to perform one or
more of its basic operations (creating a page, writing/deleting entries, ...).

## Building benchmarks

To build the benchmarks, add `//src/ledger/bin/tests/benchmark` to the list of
packages available in your build:

```sh
fx set core.x64 --release --with //src/ledger/bin/tests/benchmark
fx build ledger_benchmarks
```

## Run benchmarks

### Using trace spec files
The easiest way to run a benchmark is using the associated trace spec file,
for example:

```
trace record --spec-file=/pkgfs/packages/ledger_benchmarks/0/data/put.tspec
```

You can read more on the trace spec file format in the [trace-based benchmarking
guide](/docs/development/benchmarking/trace_based_benchmarking.md#specification-file).

### Tracing benchmark apps directly
You can also trace the app directly:

```
trace record --categories=benchmark,ledger \
  fuchsia-pkg://fuchsia.com/ledger_benchmarks#meta/put.cmx \
  --entry-count=10 --transaction-size=1 --key-size=100 --value-size=100 \
  --refs=false
```

That would generate a trace result file (default: `/data/trace.json`), which can
be analysed manually or using special tools.

### Benchmark parameters

\*.tspec files specify the parameters with which benchmark apps are run. You can
override these by passing the "--append-args" argument to the `trace record`
tool.
Commonly used among all the ledger benchmarks are the following parameters:

* `entry-count` for the number of entries to perform operations on
* `unique-key-count` if the number of operations and the number of entries
  differ (i.e. some entries are overwritten), this denotes the number of unique
  entries in the page
* `key-size` for the size of the key (in bytes)
* `value-size` for the size of the value (in bytes)
* `refs` with the values `on` or `off` for the reference strategy: if set to
  `on`, entries will be put using `CreateReference`/`PutAsReference`, otherwise
  they will be treated as inline entries.
* `commit-count` for the number of commits made to the page
* `transaction-size` for the number of operations in a single transaction

Unless the name of the benchmark suggest otherwise, default values are:
* `100` entries
* key size `100`
* value size `1000`
Benchmarks under `sync` and `convergence` use smaller number of entries and
smaller value size.

### A note regarding benchmark apps
Since the benchmark apps are designed to be used with tracing, running them
without a tracing will not generate any results.

## List of benchmarks

Some of the benchmark apps have several corresponding tspec files, to exercise
different possible scenarios of using Ledger. For example, `get_page` benchmark
is used in `add_new_page.tspec` to emulate creating many pages with different
IDs, and in `get_same_page.tspec` to create several connections to the same
page.

### Local benchmarks
* __Get page__: How long does it take to establish a new page connection?
    * `add_new_page.tspec`: connection to the new page with previously unused
      ID.
    * `add_new_page_precached.tspec`: same as above, but waits for a precached
      Ledger Page to be ready before each request.
    * `add_new_page_after_clear.tspec`: same as `add_new_page.tspec` but each
      previous page is populated with one entry, then cleared and evicted.
    * `get_same_page.tspec`: several connections to the same page
    * `get_page_id.tspec`: how long does the GetId() call takes on a newly
      created page?
* __Put__: How long does it take to write data to a page? And how long before the
  client will receive a [PageWatcher notification] about its own change?
    * `put.tspec`: basic case
    * `put_big_entry.tspec`: writing big entries to the page
    * `put_as_reference.tspec`: entries are put as references (CreateReference +
      PutReference)
    * `transaction.tspec`: changes are made in a transaction
* __Update entry__: How long does it take to update an existing value in Ledger
  (make several Put operations with the same key, but different values)?
    * `update_entry.tspec`: basic case
    * `update_big_entry.tspec`: put an entry of big size, then update its value
    * `update_entry_transactions.tspec`: changes are grouped in transactions
* __Get entry__: How long does it take to retrieve a single value from a page?
  This benchmark also measures the time taken by the GetSnapshot call and the
  GetKeys call to retrieve all keys from the page.
    * `get_small_entry.tspec`: basic case
    * `get_small_entry_inline.tspec`: GetInline is used instead of Get
    * `get_big_entry.tspec`: values of bigger size are used.
* __Delete entry__: How long does it take to delete an entry from a page?
    * `delete_entry.tspec`: each entry is deleted separately (outside of a
      transaction)
    * `delete_big_entry.tspec`: same as above, but for big entries
    * `delete_entry_transactions.tspec`: deletions are grouped in transactions
    * `disk_space_cleared_page.tspec`: how much space does ledger take after the
      page was cleared out (all the entries deleted in one transaction)?
* __Story simulation__: How long does it take to create and edit a Modular
  story?
    * `stories_single_active.tspec`: a single story is active at all times. Once
    editing is complete, the contents of the story (and any corresponding rows
    in the root page) are removed. This is the base case.
    * `stories_many_active.tspec`: same as in the base case, but keep up to 20
    pages active, before clearing their contents.
    * `stories_wait_cached.tspec`: same as in the base case, but wait for a
    precached Ledger Page before creating a new Story.
* __Disk space__: How much disk space does ledger use to store pages, objects
  and commits?
    * `disk_space_empty_ledger.tspec`: empty ledger (with no pages)
    * `disk_space_empty_pages.tspec`: ledger containing only empty pages
    * `disk_space_entries.tspec`: ledger with one page containing some entries
    * `disk_space_small_keys.tspec`: same, but with small (10 bytes) keys.
    * `disk_space_updates.tspec`: ledger with one page containing only one
      entry, but long commit history
    * `disk_space_one_commit_per_entry.tspec`: ledger with one page containing
      several entries, each of them added in a separate commit

[trace-based benchmarks]: /docs/development/benchmarking/trace_based_benchmarking.md
[cloud sync set-up instructions]: /src/ledger/docs/testing.md#cloud-sync
[lazy value]: /src/ledger/docs/api_guide.md#lazy-values
[PageWatcher notification]: /src/ledger/docs/api_guide.md#watch
