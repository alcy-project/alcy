# `alcy init`

Create a new package (or a new suite) in the directory you are already
in. The scaffold contents and behavior are those of [`alcy
new`](new.md); the only difference is where the files land: this verb
never makes a new directory for you.

## Usage

```console
$ alcy init [--vcs git|none] [--suite <name>] [directory]
```

Without an argument, the current directory becomes the package's root
and its manifest names the directory as the package's name. With a
name, `alcy init` is exactly `alcy new` with the name spelled out - and
fails the same ways.

## The content after completion

A package's manifest, a minimal `main.al`, and nothing else. `alcy.toml`
with a `[package]` entry and a `[[bin]]` that points at `main.al`; the
license line is the one thing that starts empty.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | the package exists |
| 5 | the scaffold could not be written |

## See also

- [`alcy new`](new.md) - create in a new directory
- [`alcy build`](build.md) - the next command you type