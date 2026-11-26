# Alcy command reference (Tools)

The [README](../../README.md) calls `alcy` one command for the whole
development loop. This page is that claim in detail: every subcommand,
and the parts of the command line they all carry. The compiler and the
tools that read its sources are one binary and one version, so there is
no separate formatter, linter or language server to install. Each verb
has its own page of flags, positionals, exit codes, and examples; the
language itself is described in the [specification](../spec/overview.md)
and is not repeated here.

## Commands at a glance

| Command | Purpose |
| --- | --- |
| `alcy build` | Build a package or suite; emit an object, an executable, or the IR |
| `alcy compile` | Build one source file, or standard input |
| `alcy check` | Analyze without emitting artifacts |
| `alcy test` | Build a package with its test items and run them |
| `alcy run` | Build and execute a package |
| `alcy fmt` | Reformat source files |
| `alcy lint` | Report style and design findings in source files |
| `alcy lsp` | Speak the Language Server Protocol on standard input/output |
| `alcy new` | Create a new package directory |
| `alcy init` | Create a package in an existing directory |

## Invocation basics

`alcy` reads the directory it is run in unless the first positional
argument says otherwise. A positional naming an existing directory is
the package directory verbatim; an absent positional means the current
directory. A command that takes a package or suite never takes a bare
`.al` file as its positional - use `--file` when the verb documents
it, and `compile` for a single file.

A verb that needs the argument order it was given sees it as a single
string; nothing downstream re-parses it.

## Channels

The toolchain never guesses where text lands, and the answer is stated
in this table:

| Text | Channel |
| --- | --- |
| Diagnostics, warnings, notes | standard error |
| The `run` announcement (`Running <path>`) | standard error |
| The result line, `--time-trace` output | standard output |
| The `--json` document | standard output, and nothing else |
| `lsp` protocol frames | standard input and standard output |

Color behaves per channel, not per program: redirecting one does not
change the other's styling. Text is not localized; only the
diagnostics and flag help follow `--lang`.

## Shared flags

Every verb takes these before its own arguments:

| Flag | Values | Default | Meaning |
| --- | --- | --- | --- |
| `--color` | `auto`, `always`, `never` | `auto` | Color mode for logging and diagnostics. `auto` probes the stream it writes to |
| `--lang` | `en-us` | `en-us` | Language for every message this run prints. The locale the shell exports is never consulted |
| `-t`, `--time-trace` | flag | off | Time the pipeline's phases; with `--json` the events embed in the document, on their own the phases print as a pruned tree |
| `--json` | flag | off | One JSON document on standard output, for a tool reading it. Applies to `build`, `compile`, `check`, `lint`, and `test` |
| `-j`, `--jobs` | `N` | half the machine | Threads reading source files. `--jobs 0` asks for that default; `-j 1` reads in order. Everything after the read is single-threaded |

`--release` selects the built-in `release` profile; `--profile <name>` selects
one by name (see [the profile file](build.md#the-profile-file) in
[`build`](build.md)). The two may not be given together.

## The `--json` envelope

A verb that accepts `--json` writes exactly one JSON object on
standard output and nothing else. Comments, notes, the time-trace
tree, and human sentences are fields of the same document, never
interleaved with it:

```json
{
  "status": "ok",
  "outcome": "built",
  "args": ["--release"],
  "output_path": "out/app",
  "link_sdk": null,
  "subject": "package app (1 target)",
  "cache": "miss",
  "diagnostics": [],
  "stats": {
    "wall_ns": 147300000,
    "peak_memory_bytes": 480000000,
    "modules": 3,
    "functions": 120,
    "files": 3
  },
  "traceEvents": []
}
```

Exit codes and JSON are one contract: a tool that reads the document
ignores the exit code, a script that reads neither stays broken. Use
`--json` when you are programming against Alcy; use the exit code when
you are scripting it.

## Exit codes

The values are stable. Existing codes are never renumbered; a new
command gets its own value.

| Code | Name | When |
| --- | --- | --- |
| 0 | success | The command ran to completion |
| 1 | parse error / test failed | The command line was rejected, or a `test` run saw a test fail |
| 3 | build failed | `build` or `compile` could not produce its output |
| 4 | check failed / test unrunnable | `check` found errors, or the package was not a program `test` can accept |
| 5 | new failed | `new` or `init` could not create the package |
| 6 | run failed | `run` could not build or the program was not accepted |
| 7 | fmt failed | `fmt` could not read or write a file |
| 8 | fmt not clean | `fmt --check` found a file whose layout would change |
| 9 | lint failed | A denied lint finding, or `lint` could not read its input |

`lsp` does not return a meaningful exit code on its happy path: it
serves until the client closes the channel.

## The package model

A package is a directory with an `alcy.toml`. A suite is a directory
with an `alcy.toml` whose `[suite]` table names member paths, one
name per package. Neither is required to recompile anything:
`build`/`run`/`check`/`test` read the nearest ancestor manifest,
or the positional directory you gave them.

A positional package applies to that package. A positional suite applies
to every listed member: `build`, `check`, `lint`, `fmt`, and `test` walk
the members in declared order, and `test` names the member in each test
label. `run` needs an explicit choice, because a suite does not say which
of its programs a reader means: add `[suite] default_run` listing the
member paths to run in order. Without it, or with an empty list, `run`
stops rather than guessing.

The trailing operand of `new` names what to create: a package name, an
owner/name pair, or (with `--suite`) a suite name in the same grammar.
Subdirectory paths create the directories they name; the last segment
is the package name.

## `.alcy/` directory

`.alcy/` beside the goal package's manifest holds the files that are not
the program's own description of itself. Only the goal package's
`.alcy/` is ever read; a dependency's is invisible.

| File | Holds |
| --- | --- |
| `toolchain.toml` | Link driver and arguments, `freestanding` |
| `profile.toml` | Named build profiles and the keys that override the built-in ones |
| `lint.toml` | The linter's levels, options, and `borrow-checks` |

The key grammar inside each file is on the page of the command that
reads it: [`build`](build.md), [`lint`](lint.md).

## Configuration and environment

Where a build setting comes from is part of the command's contract:

- Build settings come from the command line or the project's files.
  `--lang` chooses the message language and `NO_COLOR` only turns color
  off; neither changes an artifact. One known exception remains: a
  native executable's link resolves the host's C runtime and SDK,
  which are discovered from the host installation and its platform
  SDK environment variables. Only that artifact is not reproducible
  across machines. Objects, assembly, IR, and wasm modules do not link
  and are; the host path a native link resolved is reported as
  `link_sdk` in `--json` - an SDK or kit directory on macOS and
  Windows, the startup-file directory on Linux.
- `compile` reads no configuration file. It never consults `alcy.toml`
  or `.alcy/`, so one command line is the whole story. Its profile and
  toolchain choices are built in or given as flags.
- `build` reads the goal package's `alcy.toml`, which declares what is
  built, and its `.alcy/` files, which say how. A flag overrides any
  key in a `.alcy/` profile or toolchain file. A dependency's `.alcy/`
  is never read.
- The two commands accept the same build and emission flags, including
  `--emit`, `--backend`, and `--freestanding`. `compile`'s `--stdin`,
  `--no-std`, and `--deps` exist because its input has no package;
  a package states those choices in its manifest.

## Where things live

- `docs/spec/` - the normative language. Rules a program can observe.
- `docs/guide/` - reading order for humans new to Alcy.
- `docs/tools/` - this page: the command line itself.
- `src/docs/` - how the compiler is put together.

## See also

- [Backends and output formats](backends.md)
- [The getting-started guide](../guide/en/02-getting-started.md)
- `alcy --help` on your machine - the authoritative synopsis for the build you
  have
