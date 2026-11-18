# `alcy build`

Build the package in the current directory (or the one you name).
`alcy build` is the verb you run after `alcy check` says the source
is correct and you want to run it; it produces the artifact, writes
it beside the manifest, and tells you where it went.

## Usage

```console
$ alcy build [--release | --profile <NAME>] [--emit MODE]
             [--backend NAME] [--target TRIPLE]
             [--output PATH] [--linker DRIVER] [--link-args ARG]...
             [--file FILE] [--no-cache] [PACKAGE-DIR]
```

`PACKAGE-DIR` defaults to the current directory. A positional that
ends in `.al` is rejected - use `--file` or `alcy compile`.

## What it reads

`alcy build` reads the nearest `alcy.toml` under the argument
(including the argument). Its `[[bin]]` and `[lib]` entries are what
get built; `[dependencies]` are resolved closed to the package. A
suite (`alcy.toml` with a `[suite]` table naming members) builds
every listed member.

For a package with several targets, everything is built; the positional
names what to build next to it when that is useful. `--file` builds
exactly that one source from command-line state alone - the package's
manifest and its `.alcy/` directory are not consulted.

## Output placement

Unless you name one, the artifact lands beside the manifest it was
built from and is named after the target that produced it:

| Case | Artifact location |
| --- | --- |
| Package with one `[[bin]]` | `<package>/out/<name>` |
| Package with `[lib]` | `<package>/out/<name>.o` |
| Suite | `<suite>/out/<member>/<name>` per member |

`--output` names one artifact, so it is rejected for a suite and
rejected for a package with more than one target. `-o -` means stop
and fail like any other invalid value - the compiler never streams an
artifact it cannot verify.

## Flags

### Profile

| Flag | Values | Default | Meaning |
| --- | --- | --- | --- |
| `--release` | flag | off | The built-in `release` profile. The same as `--profile release` |
| `--profile` | name | `debug` | A profile by name: built-in `debug`/`release`, or one `.alcy/profile.toml` declares |
| `--on-panic` | `abort`, `backtrace` | `abort` | What a panic does before the process ends; overrides the profile's `on-panic` |
| `--optimize` | `on`, `off` | on | Run the optimizer; overrides the profile's `optimize` |
| `--overflow-checks` | `on`, `off` | on | Abort on integer overflow; overrides the profile's `overflow-checks` |

`--release` and `--profile` may not be given together.

### Output

| Flag | Values | Default | Meaning |
| --- | --- | --- | --- |
| `-o`, `--output` | path | (per table above) | Literal output path |
| `--emit` | `executable`, `object`, `llvm-ir`, `llvm-bc`, `ir`, `ir-bc` | `executable` | What to write. An unnamed file replaces the suffix: `foo.al` -> `foo.o`, `foo.ir`, `foo.bc`, `foo.ll`, `foo.irb`; the executable keeps its platform's suffix (none on POSIX, `.exe` on Windows; `.wasm` for a wasm target) |
| `--backend` | `llvm`, `direct-wasm` | built-in | Which backend writes the code. `--emit=ir`/`--emit=ir-bc` need no backend at all and ignore the flag |
| `--target` | `<triple>` | `host` | The machine to build for (`wasm32-unknown-emscripten` is a real target). `--emit` chooses the artifact kind, not this |
| `--no-cache` | flag | off | Neither read nor write the build cache for this invocation |

### Linking

| Flag | Values | Default | Meaning |
| --- | --- | --- | --- |
| `--linker` | driver | (default) | System linker driver for executable builds; overrides `.alcy/toolchain.toml` |
| `--link-args` | argument | (empty) | Argument for the link driver after the objects. Repeatable, one element each; overrides `.alcy/toolchain.toml` |

### `.alcy/toolchain.toml`

```toml
linker = "ld.lld"
link-args = ["-Wl,-z,now"]
freestanding = true
```

`freestanding = true` links without the C runtime: no startup files,
no libc, and the compiler emits `_start` as the entry instead of
`main`. A freestanding build's `main` keeps its signature and its
value becomes the process's exit code.

### The profile file

```toml
[debug]
on-panic = "backtrace"
overflow-checks = true

[release]
on-panic = "abort"
optimize = true

[fast]
optimize = true
overflow-checks = false
```

Known keys are `on-panic` (`abort` or `backtrace`), `optimize`, and
`overflow-checks`, and the value grammar for booleans is the TOML-native
`true`/`false`. A declared `[fast]` is what `alcy build --profile fast`
selects. A package may override only the three built-in keys in the
built-in tables; a table of its own may use them with their own values,
or derive ones it names from defaults.

## Examples

```console
$ alcy build
Built     package hello (1 target)  (4.3 KiB, 28.6 ms, 96.9 MiB peak, cache miss)

$ alcy build --json
{"version":1,"command":"build","status":"ok","outcome":"built","cache":"miss","summary":"Built     package hello (1 target)  (4.3 KiB, 23.4 ms, 96.6 MiB peak, cache miss)","output_path":"out/hello","subject":"package hello (1 target)","stats":{"files":0,"modules":0,"functions":120,"findings":0,"fixes":0,"tests":0,"test_failures":0,"test_ended_early":false,"output_bytes":4416,"wall_ns":23438168,"peak_memory_bytes":101269504},"diagnostics":[]}

$ ls out/
hello
```

`alcy build` borrows no target name between the file it produces and
the report it gives; exit code `3` means the build failed and the bag
says why.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 3 | the build failed. Diagnostics on standard error carry the reason |

## See also

- [`alcy compile`](compile.md) - a single file, no package
- [`alcy check`](check.md) - analyze without emitting
- [`alcy run`](run.md) - build and execute
- `alcy build --help` - the synopsis for this build
