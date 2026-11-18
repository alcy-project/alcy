# `alcy fmt`

Reformat alcy source to the compiler's canonical layout. `fmt` is the
same on every machine for every input and does not consult an
environment variable, a config file, or a terminal's capabilities.

## Usage

```console
alcy fmt [--file FILE] [--stdin] [--stdout] [--check] [--diff] [--exclude SUFFIX]... [PACKAGE-DIR]
```

Three shapes the command takes:

| Invocation | What happens |
| --- | --- |
| `alcy fmt` | The package's sources are reformatted in place |
| `alcy fmt --file FILE` | That one file, in place |
| `alcy fmt --stdin --stdout` | The one file from standard input is written to standard output |
| `alcy fmt --check` | Every file is compared against the canonical layout; the report goes to standard error |
| `alcy fmt --diff` | The same comparison, printed as a unified diff |

`--check`, `--diff`, `--stdin`, and `--stdout` compose:
`--stdin --stdout --diff` prints a diff and writes nothing.

## What it reads

Every source file in the package the positional names, excluding
dependencies and anything outside the module list. With `--stdin` the
input is standard input, and the name it is reported under is
`<stdin>`.

A file the compiler cannot read as source is reported, not skipped.

## Options

| Flag | Values | Meaning |
| --- | --- | --- |
| `--file FILE` | path | Format (or compare) only that file |
| `--stdin` | flag | Read the file from standard input. Incompatible with a positional directory |
| `--stdout` | flag | Write one file's result to standard output instead of to the file |
| `--check` | flag | Print the files whose layout would change; writes nothing |
| `--diff` | flag | Show the unified diff instead of writing. A change set too scattered to line up is shown as a wholesale replacement |
| `--exclude SUFFIX` | text | Skip files whose path ends with this suffix; repeatable |

`alcy fmt` writes nothing else: there is no width knob, no quote
style, no spacing amount. One layout per input.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | files were written, or nothing needed to be (`--check` passed) |
| 7 | a file could not be read or written |
| 8 | `--check` found a file whose layout would change |

## Examples

```console
$ alcy fmt
Formatted 2 files  (3.59 ms, 65.8 MiB peak)
```

```console
$ alcy fmt --check
Checked   2 files,   (2.29 ms, 65.6 MiB peak)
$ echo $?
0
```

```console
$ alcy fmt --check
main.al needs formatting
$ echo $?
8
```

`main.al needs formatting` is the line standard error carries; the
exit code is the question a CI step asks.

## See also

- [`alcy lint`](lint.md) - findings that are not layout
- `alcy fmt --help` - the synopsis for this build
