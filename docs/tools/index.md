# alcy command reference (Tools)

The `alcy` command line is the whole toolchain: it builds packages and
single files, checks them, runs them, tests them, and serves the
language server until your editor closes. This page is the shared
reference - every subcommand and the parts of the command line they
all carry. Each verb has its own page of flags, positionals, exit
codes, and examples; the language itself is described in the
[specification](../spec/overview.md) and is not repeated here.

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
| `-j`, `--jobs` | `N` | `1` | Threads reading source files. `--jobs 0` is refused; everything after the read is single-threaded |

`--release` selects the built-in `release` profile; `--profile <name>`
selects one by name (see [the profile file](build.md#profiletoml)). The
two may not be given together.

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
`--json` when you are programming against alcy; use the exit code when
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

## Environment rules

The toolchain does not read the environment for behavior. The locale
is not consulted, the set of variables the shell exports changes
nothing about what a command accepts or selects, and no exit code
depends on them. One command is exact: `new` writes a `.gitignore`
because you asked for `git`. Nothing else is a hint, a default, or a
feature.

## Where things live

- `docs/spec/` - the normative language. Rules a program can observe.
- `docs/guide/` - reading order for humans new to alcy.
- `docs/tools/` - this page: the command line itself.
- `src/docs/` - how the compiler is put together.

## See also

- [The getting-started guide](../guide/02-getting-started.md)
- `alcy --help` on your machine - the authoritative synopsis for the build you have