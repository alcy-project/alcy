# `alcy new`

Create a new package (or a new suite) in the directory the argument
names. `init` is the same scaffold written into the directory you are
already in; the two differ only in where the files land.

## Usage

```console
$ alcy new [--vcs git|none] [--suite <name>] <directory>
```

The trailing operand names a directory path; the package's name is the
last segment of that path. Nesting works the same way: `alcy new
core/data` creates `core/data/`.

| Invocation | Result |
| --- | --- |
| `alcy new my-lib` | A package named `my-lib` in `./my-lib/` |
| `alcy new core/data` | Creates `core/data/` with the directories it needs |
| `alcy new my-lib/pack` | A package named `pack` in `core/pack/` |

## Scaffold contents

```toml
[package]
name = "my-lib"
version = "0.1.0"
license = ""

[modules]
include = ["main"]

[dependencies]
"alcy/std/*" = {}

[[bin]]
name = "my-lib"
path = "main.al"
```

Plus a minimal `main.al`, and a `.gitignore` naming `out/` and nothing
else when `--vcs git` is selected (the default). `--vcs none` writes
no ignore file. No repository is created.

A package created under an existing suite's manifest is added to the
suite's `packages`, preserving the rest of the file.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | the package exists |
| 5 | the scaffold could not be written |

## See also

- [`alcy init`](init.md) - the same scaffold in the current directory
- [`alcy build`](build.md) - the next command you type