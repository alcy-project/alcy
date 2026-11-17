# `alcy check`

Analyze a package or suite and stop before writing any artifact.
`check` runs the whole frontend - parse, resolve, type-check, drop
elaboration, borrow check - and reports what it finds. It is the
command you run in your editor's save hook and the one a CI check
step calls; it never produces an object file and never touches the
linker.

## Usage

```console
$ alcy check [--file FILE] [PACKAGE-DIR]
```

`PACKAGE-DIR` defaults to the current directory. `.al`-suffixed
positionals are rejected - use `--file` for a single source.

## Targets

| Form | Meaning |
| --- | --- |
| `alcy check` | The package or suite in the current directory |
| `alcy check path/to/pkg` | The package or suite at the given path |
| `alcy check --file main.al` | That one source file - the package is not consulted |

`--file FILE` names one source directly; the package directory is
ignored. A positional and `--file` together are rejected, because a
run that analyzed two roots with two outcomes could only report the
wrong one.

## What it verifies

- The lexer and parser produce a well-formed tree.
- Name resolution binds every path.
- Type checking infers and checks every body.
- Drop elaboration and the borrow check pass over every lowered function.
- A `test {}` block in the package is never analyzed for `check` unless it is a package's own test.

A package with an error the language rules do not allow cannot slip
past `check`; it reports everything in one run, in the order analysis
reaches it, and its exit code is the gate.

## Examples

```console
$ alcy check
Checked   1 file, 1 module, 1 function  (3.46 ms, 71.8 MiB peak)
$ alcy check --json
{"version":1,"command":"check","status":"ok","outcome":"checked","cache":"off","summary":"Checked   1 file, 1 module, 1 function  (2.92 ms, 71.7 MiB peak)","output_path":null,"stats":{"files":1,"modules":1,"functions":1,"findings":0,"fixes":0,"tests":0,"test_failures":0,"test_ended_early":false,"output_bytes":0,"wall_ns":2916373,"peak_memory_bytes":75165696},"diagnostics":[]}
```

Diagnostics of the form `error[ECxxx]`/`warning[WCxxx]` are the same
spans and codes `build` would report; the span format is
`--> file:offset+length` with the snippet beside it. The diagnostic
codes are named by the stage that emitted them, not by a stable
document index - the document you are reading now is the reference.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 4 | the analysis found an error |

## See also

- [`alcy build`](build.md) - emit an artifact
- [`alcy lint`](lint.md) - warnings and style the compiler does not enforce
- `alcy check --help` - the synopsis for this build