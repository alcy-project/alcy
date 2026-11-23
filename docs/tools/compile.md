# `alcy compile`

Compile one source file to an artifact. `compile` is the command the
language tools, CI scripts, and the playground reach for when they do
not have a package to work with. It never reads `alcy.toml` or any
`.alcy/` file, so one command line is the whole story: an input, the
options, and the output.

## Usage

```console
$ alcy compile FILE [--release | --profile <NAME>] [--emit MODE]
                 [--asm-syntax att|intel] [--backend NAME] [--target TRIPLE]
                 [--output PATH]
                 [--linker DRIVER] [--link-args ARG]... [--freestanding]
                 [--stdin] [--no-std] [--deps FRAGMENT]...
```

`FILE` defaults to absent only when `--stdin` is the input; without
either, the command refuses before assembling a command line.

## Input

| Input | What reads |
| --- | --- |
| `FILE` | The single `.al` file, or a fragment `.al`-suffixed |
| `--stdin` | Standard input, reported against `<stdin>` |

`--stdin` takes no positional input; `--stdin` and a file are
incompatible. An input without `.al` suffix is still a valid target -
the suffix never decides what parses.

## Output

| Mode | Naming if you do not say |
| --- | --- |
| `executable` (default) | With one input: the input with the last extension replaced by the platform's suffix (none on POSIX; `.exe` on Windows; `.wasm` for a wasm target); with `--stdin`: required, otherwise refused |
| `object` | Input: replace the extension with `.o`; `--stdin`: required |
| `asm` | Input: replace with `.s`; textual assembly, no linking |
| `llvm-ir` | Input: replace with `.ll`; needs no linking |
| `llvm-bc` | Input: replace with `.bc` |
| `ir` | Input: replace with `.ir` |
| `ir-bc` | Input: replace with `.irb` |

`--output` names the file; without one beside the input, `--stdin`
needs it too. `--emit=ir`/`--emit=ir-bc` need no target or backend at
all: they serialize the lowered package for `comp` opts-outs and
tools, so a build with no LLVM still answers either.

## Flags

| Flag | Values | Default | Meaning |
| --- | --- | --- | --- |
| `-o`, `--output` | path | (per table) | Literal output path. With `--stdin`, required |
| `--emit` | `executable`, `object`, `asm`, `llvm-ir`, `llvm-bc`, `ir`, `ir-bc` | `executable` | Artifact kind to write; replaces, never appends an extension |
| `--asm-syntax` | `att`, `intel` | `att` | The spelling `--emit=asm` writes. LLVM prints its own assembly and ignores it |
| `--backend` | `llvm`, `direct-wasm`, `direct-x86_64` | native x86_64 Linux debug: `direct-x86_64`; otherwise `llvm` | Which backend writes the code. See [Backends and output formats](backends.md) |
| `--target` | `<triple>` | `host` | The machine to build for |
| `--linker` | driver | system default | System linker driver for executable builds |
| `--link-args` | argument | (empty) | Argument for the linker. Repeatable |
| `--freestanding` | flag | off | Link without the C runtime and enter at `_start` |
| `--no-std` | flag | off | Start with no standard library selection |
| `--deps` | fragment | (empty) | A dependency on top of the default suite, spelled as a manifest specifier or `specifier = { ... }`. Repeatable |
| `--release` | flag | off | The built-in `release` profile |
| `--profile` | name | `debug` | Built-in or the name of a built-in profile; `.alcy/profile.toml` is not read |
| `--on-panic` | `abort`, `backtrace` | `abort` | Panic behavior; overrides the profile |
| `--optimize` | `on`, `off` | on | Run the optimizer |
| `--overflow-checks` | `on`, `off` | on | Abort on integer overflow |

`--profile` for a package's own `[fast]` means there is no such profile:
`compile` takes no `.alcy/`, so `--profile fast` is an unknown profile.

## Examples

```console
$ alcy compile examples/collatz.al
Compiled  examples/collatz  (129.1 KiB, 812 ms, 352 MiB peak, cache miss)

$ alcy compile --emit object examples/collatz.al
```

```console
$ echo 'fn main() -> i32 { ret 42 }' | alcy compile --stdin -o out.bin
Compiled  out.bin  (129.1 KiB, 760 ms, 350 MiB peak)
```

The exit code is `3` when nothing could be compiled, and diagnostics
land on standard error; the result line and `--json` go to standard
output. `alcy compile --json` writes the same document `build` does.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 3 | the compile failed. Diagnostics on standard error carry the reason |

## See also

- [`alcy build`](build.md) - the package verb; reads `.alcy/`
- [`alcy check`](check.md) - analyze without emitting
- `alcy compile --help` - the synopsis for this build
