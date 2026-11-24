# `alcy run`

Build the package and execute it. `alcy run` does everything
`alcy build` does to reach a linked program, `exec`s it, and lets its
exit status stand.

## Usage

```console
$ alcy run [--release | --profile <NAME>] [--linker DRIVER] [--link-args ARG]...
         [--on-panic abort|backtrace] [--optimize on|off] [--overflow-checks on|off]
         [--no-cache] [--freestanding] [PACKAGE-DIR] [-- ARGS...]
```

`PACKAGE-DIR` defaults to the current directory. A positional ending
in `.al` is rejected; `compile` is the command for one file.

## The program arguments

Everything after `--` is the program's argument list, in order, as
written. Nothing before it is treated as the program's own operand,
so `alcy run -- arg1 arg2` passes `arg1` and `arg2` and never as the
package's directory.

## What it runs

`alcy run` announces the program being run on standard error:

```console
$ alcy run
Running   out/hello
```

A `Running ...` line goes to standard error even when standard output is
redirected, so a pipeline parses the program's output cleanly.

A suite runs the members named by `[suite] default_run`, in declared
order. Without that key, or with an empty list, `alcy run` stops with an
error rather than guessing which programs you meant. Explicit member
paths still run one program each.

## Builds the run keeps

| Mode | Behavior |
| --- | --- |
| package with an entry | built, then `exec`ed |
| library-only package | no entry, so the run fails without an artifact to point at |
| suite with `default_run` | each listed member's program runs in order |
| suite without `default_run` | refused: no program was named |

## Panics

| Profile/flag | What a panic does |
| --- | --- |
| `abort` (default) | Writes the panic line and aborts |
| `backtrace` | Also walks the frame chain and names the frames it can see |

```console
$ alcy run --on-panic backtrace
Running   panic
panic at main.al:7:5: boom
  at inner (main.al:7:5)
  at main (main.al:2:7)
```

`--on-panic` overrides the profile's `on-panic` key.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | the program exited with `0`. The exit code of the program itself is at the shell, not here |
| 6 | the package was not a program `alcy run` can launch (build error, missing entry, a suite, a library) |
| the program's own exit code | otherwise; for a suite, the first failing program's code |

For a suite, the runs continue after a failing program. A line on standard
error says how many programs ran and how many failed.

## See also

- [`alcy build`](build.md) - the artifact without running it
- [`alcy check`](check.md) - analyze without emitting
- `alcy run --help` - the synopsis for this build
