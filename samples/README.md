# Samples

Programs written in alcy, kept because they are worth reading. Each is a
directory holding a `main.al` and an `expect.toml`, which is the same
shape as a case under `exe/cases`, and the same harness runs them:

```sh
uv run ./tools/check_exe.py --cases-root=samples
```

Sharing the harness is the point. A sample that stops compiling, or that
prints something other than what it claims, fails the build — so these
are examples that cannot quietly rot.

| Sample | What it shows |
| --- | --- |
| `fizzbuzz` | `while`, `mut`, `else if`, and a single-argument `format` |
| `fibonacci` | a named function, iterative, printing a formatted table |
| `primes` | two functions, an early `ret`, and trial division to a square root |

## What they do not show

They are not a tour of the language. Several things alcy has are absent
here, most obviously collections: there is no `Array` or `String` to push
into, so every sample prints line by line. That is also why these are
small — the exercises are the ones a reader already knows the shape of,
so the reading is about alcy rather than about the algorithm.

## Writing one

Add a directory with a `main.al` and an `expect.toml`. `exit` is
required; `stdout`, `stdout_contains`, and `stderr_contains` are optional.
The `stdout` block is TOML, so it is flush-left — TOML keeps the
indentation of a multi-line string, and an indented one would compare
against output that has no leading spaces:

```toml
exit = 0
stdout = """
2
3
"""
```
